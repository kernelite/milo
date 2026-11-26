#include "fs/devfs/devfs.hpp"

#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/AddressSpace.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"
#include "sync/IrqGuard.hpp"
#include "uapi/sys.hpp"
#include "uapi/termios.hpp"

namespace Kernel::FS {

namespace {

constexpr uint16_t kDefaultTtyRows = 24U;
constexpr uint16_t kDefaultTtyCols = 80U;
constexpr size_t kTtyRxBufSize = 256U;

constexpr linux_ktermios default_termios() noexcept {
    return linux_ktermios{
        .c_iflag = Kernel::UAPI::ICRNL,
        .c_oflag = Kernel::UAPI::OPOST | Kernel::UAPI::ONLCR,
        .c_cflag = Kernel::UAPI::CS8 | Kernel::UAPI::CREAD | Kernel::UAPI::CLOCAL,
        .c_lflag = Kernel::UAPI::ISIG | Kernel::UAPI::ICANON | Kernel::UAPI::ECHO |
                   Kernel::UAPI::ECHOE | Kernel::UAPI::ECHOK,
        .c_line = 0U,
        .c_cc = {0x03U, 0x1CU, 0x7FU, 0x15U, 0x04U, 0x00U, 0x01U, 0x00U, 0x11U, 0x13U, 0x1AU, 0x00U,
                 0x12U, 0x0FU, 0x17U, 0x16U, 0x00U, 0x00U, 0x00U},
    };
}

void wait_for_tty_event() noexcept {
    auto& cpu = HAL::get_cpu();
    Kernel::Scheduler::yield();
    cpu.enable_interrupts();
    cpu.halt();
    cpu.disable_interrupts();
}

class ConsoleTty final {
  public:
    constexpr ConsoleTty() noexcept = default;
    ~ConsoleTty() noexcept = default;

    ConsoleTty(const ConsoleTty&) = delete;
    ConsoleTty& operator=(const ConsoleTty&) = delete;
    ConsoleTty(ConsoleTty&&) = delete;
    ConsoleTty& operator=(ConsoleTty&&) = delete;

    void reset() noexcept {
        m_termios = default_termios();
        m_foreground_pgid = 1U;
        flush_rx_unlocked();
    }

    void poll_rx() noexcept {
        const Kernel::Sync::IrqGuard irq_guard{};
        pump_uart_unlocked();
    }

    void set_foreground_pgid(uint32_t pgid) noexcept {
        if (pgid > 0U) {
            m_foreground_pgid = pgid;
        }
    }

    [[nodiscard]] uint32_t foreground_pgid() const noexcept { return m_foreground_pgid; }

    [[nodiscard]] int64_t read(File* file, void* buf, size_t count) noexcept {
        if (file == nullptr || (count > 0U && buf == nullptr)) {
            return kEinval;
        }
        if (count == 0U) {
            return 0;
        }

        auto* const proc = Kernel::Process::Process::current();
        const bool nonblock = (file->get_flags() & kOpenNonBlock) != 0U;
        const bool canonical = (m_termios.c_lflag & Kernel::UAPI::ICANON) != 0U;
        const bool echo = (m_termios.c_lflag & Kernel::UAPI::ECHO) != 0U;
        const bool echoe = (m_termios.c_lflag & Kernel::UAPI::ECHOE) != 0U;
        const bool echok = (m_termios.c_lflag & Kernel::UAPI::ECHOK) != 0U;

        auto* const out_buf = static_cast<char*>(buf);
        size_t total_read = 0U;

        if (!canonical) {
            const uint8_t vmin = m_termios.c_cc[Kernel::UAPI::VMIN];
            const uint8_t vtime = m_termios.c_cc[Kernel::UAPI::VTIME];
            const size_t target_min = (vmin < count) ? vmin : count;
            const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
            const uint64_t timeout_ticks = (vtime > 0U && freq > 0ULL)
                                               ? ((static_cast<uint64_t>(vtime) * freq) / 10ULL)
                                               : 0ULL;
            uint64_t deadline_ticks =
                (timeout_ticks > 0ULL)
                    ? (HAL::QemuVirt::GenericTimer::counter_ticks() + timeout_ticks)
                    : 0ULL;

            while (total_read < count) {
                uint8_t raw_byte = 0U;
                while (total_read < count && try_pop_byte(raw_byte)) {
                    bool skip_char = false;
                    const char chr = translate_input_char(static_cast<char>(raw_byte), skip_char);
                    if (skip_char) {
                        continue;
                    }
                    out_buf[total_read++] = chr;
                    if (echo) {
                        HAL::get_console().putc(chr);
                    }
                    if (timeout_ticks > 0ULL) {
                        deadline_ticks =
                            HAL::QemuVirt::GenericTimer::counter_ticks() + timeout_ticks;
                    }
                }

                if (nonblock) {
                    if (total_read > 0U) {
                        break;
                    }
                    if (proc != nullptr && Kernel::Process::has_unblocked_pending_signal(*proc)) {
                        return kEintr;
                    }
                    return kEagain;
                }

                if (vmin == 0U && vtime == 0U) {
                    break;
                }
                if (vmin > 0U && total_read >= target_min) {
                    break;
                }
                if (vmin == 0U && vtime > 0U) {
                    if (total_read > 0U ||
                        HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks) {
                        break;
                    }
                } else if (vmin > 0U && vtime > 0U && total_read > 0U) {
                    if (HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks) {
                        break;
                    }
                }

                if (proc != nullptr && Kernel::Process::has_unblocked_pending_signal(*proc)) {
                    return (total_read > 0U) ? static_cast<int64_t>(total_read) : kEintr;
                }

                wait_for_tty_event();
            }

            return static_cast<int64_t>(total_read);
        }

        bool line_done = false;
        while (total_read < count && !line_done) {
            uint8_t raw_byte = 0U;
            if (!try_pop_byte(raw_byte)) {
                if (total_read > 0U) {
                    break;
                }
                if (proc != nullptr && Kernel::Process::has_unblocked_pending_signal(*proc)) {
                    return kEintr;
                }
                if (nonblock) {
                    return kEagain;
                }
                wait_for_tty_event();
                continue;
            }

            if (raw_byte == m_termios.c_cc[Kernel::UAPI::VEOF]) {
                break;
            }

            if (raw_byte == m_termios.c_cc[Kernel::UAPI::VKILL]) {
                while (total_read > 0U) {
                    --total_read;
                    if (echo && echok) {
                        HAL::get_console().write("\b \b", 3U);
                    }
                }
                continue;
            }

            if (raw_byte == '\b' || raw_byte == 0x7FU ||
                raw_byte == m_termios.c_cc[Kernel::UAPI::VERASE]) {
                if (total_read > 0U) {
                    --total_read;
                    if (echo) {
                        if (echoe) {
                            HAL::get_console().write("\b \b", 3U);
                        } else {
                            HAL::get_console().putc('\b');
                        }
                    }
                }
                continue;
            }

            bool skip_char = false;
            const char chr = translate_input_char(static_cast<char>(raw_byte), skip_char);
            if (skip_char) {
                continue;
            }

            out_buf[total_read++] = chr;

            if (chr == '\n') {
                if (echo) {
                    HAL::get_console().write("\r\n", 2U);
                }
                line_done = true;
            } else if (echo) {
                const auto byte_val = static_cast<uint8_t>(chr);
                if (byte_val >= 0x20U && byte_val <= 0x7EU) {
                    HAL::get_console().putc(chr);
                }
            }
        }

        return static_cast<int64_t>(total_read);
    }

    [[nodiscard]] int32_t ioctl(uint64_t request, uint64_t arg) noexcept {
        auto* const proc = Kernel::Process::Process::current();
        const uintptr_t root_paddr = (proc != nullptr)
                                         ? proc->address_space().root_paddr()
                                         : Kernel::MM::AddressSpace::current_root_paddr();
        const auto user_arg = static_cast<uintptr_t>(arg);

        switch (request) {
        case Kernel::UAPI::TCGETS: {
            const int64_t res =
                Kernel::MM::copy_to_user(root_paddr, user_arg, &m_termios, sizeof(m_termios));
            return (res < 0) ? kEfault : kOk;
        }

        case Kernel::UAPI::TCSETS:
        case Kernel::UAPI::TCSETSW:
        case Kernel::UAPI::TCSETSF: {
            linux_ktermios updated{};
            const int64_t copy_res =
                Kernel::MM::copy_from_user(root_paddr, &updated, user_arg, sizeof(updated));
            if (copy_res < 0) {
                return kEfault;
            }
            {
                const Kernel::Sync::IrqGuard irq_guard{};
                m_termios = updated;
                if (request == Kernel::UAPI::TCSETSF) {
                    flush_rx_unlocked();
                }
            }
            return kOk;
        }

        case Kernel::UAPI::TIOCGWINSZ: {
            constexpr linux_winsize default_ws{
                .ws_row = kDefaultTtyRows,
                .ws_col = kDefaultTtyCols,
                .ws_xpixel = 0U,
                .ws_ypixel = 0U,
            };
            const int64_t res =
                Kernel::MM::copy_to_user(root_paddr, user_arg, &default_ws, sizeof(default_ws));
            return (res < 0) ? kEfault : kOk;
        }

        case Kernel::UAPI::TIOCGPGRP: {
            const auto pgrp = static_cast<int32_t>(m_foreground_pgid);
            const int64_t res = Kernel::MM::copy_to_user(root_paddr, user_arg, &pgrp, sizeof(pgrp));
            return (res < 0) ? kEfault : kOk;
        }

        case Kernel::UAPI::TIOCSPGRP: {
            int32_t new_pgrp = 0;
            const int64_t copy_res =
                Kernel::MM::copy_from_user(root_paddr, &new_pgrp, user_arg, sizeof(new_pgrp));
            if (copy_res < 0) {
                return kEfault;
            }
            if (new_pgrp <= 0) {
                return kEinval;
            }
            m_foreground_pgid = static_cast<uint32_t>(new_pgrp);
            return kOk;
        }

        default:
            return kEinval;
        }
    }

    [[nodiscard]] int16_t poll(uint32_t acc, int16_t events) noexcept {
        const bool readable = (acc == kOpenReadOnly || acc == kOpenReadWrite);
        const bool writable = (acc == kOpenWriteOnly || acc == kOpenReadWrite);

        const auto event_mask = static_cast<uint16_t>(events);
        constexpr auto kInMask =
            static_cast<uint16_t>(static_cast<uint16_t>(Kernel::UAPI::POLLIN) |
                                  static_cast<uint16_t>(Kernel::UAPI::POLLRDNORM));
        constexpr auto kOutMask =
            static_cast<uint16_t>(static_cast<uint16_t>(Kernel::UAPI::POLLOUT) |
                                  static_cast<uint16_t>(Kernel::UAPI::POLLWRNORM));

        uint16_t revents = 0U;
        if (readable && rx_available() > 0U) {
            revents |= static_cast<uint16_t>(event_mask & kInMask);
        }
        if (writable) {
            revents |= static_cast<uint16_t>(event_mask & kOutMask);
        }
        return static_cast<int16_t>(revents);
    }

  private:
    void flush_rx_unlocked() noexcept {
        m_rx_head = 0U;
        m_rx_tail = 0U;
        m_rx_count = 0U;
    }

    void pump_uart_unlocked() noexcept {
        auto& console = HAL::get_console();
        while (console.has_input() && m_rx_count < kTtyRxBufSize) {
            const uint8_t chr = console.getc();
            if ((m_termios.c_lflag & Kernel::UAPI::ISIG) != 0U) {
                if (chr == m_termios.c_cc[Kernel::UAPI::VINTR]) {
                    if ((m_termios.c_lflag & Kernel::UAPI::ECHO) != 0U) {
                        console.write("^C\r\n", 4U);
                    }
                    flush_rx_unlocked();
                    Kernel::Process::signal_process_group(m_foreground_pgid,
                                                          Kernel::Process::SIG_INT);
                    continue;
                }
                if (chr == m_termios.c_cc[Kernel::UAPI::VQUIT]) {
                    if ((m_termios.c_lflag & Kernel::UAPI::ECHO) != 0U) {
                        console.write("^\\\r\n", 4U);
                    }
                    flush_rx_unlocked();
                    Kernel::Process::signal_process_group(m_foreground_pgid,
                                                          Kernel::Process::SIG_QUIT);
                    continue;
                }
            }
            m_rx_buf[m_rx_tail] = chr;
            m_rx_tail = (m_rx_tail + 1U) % kTtyRxBufSize;
            ++m_rx_count;
        }
    }

    [[nodiscard]] size_t rx_available() noexcept {
        const Kernel::Sync::IrqGuard irq_guard{};
        pump_uart_unlocked();
        return m_rx_count;
    }

    [[nodiscard]] bool try_pop_byte(uint8_t& out_byte) noexcept {
        const Kernel::Sync::IrqGuard irq_guard{};
        pump_uart_unlocked();
        if (m_rx_count == 0U) {
            return false;
        }
        out_byte = m_rx_buf[m_rx_head];
        m_rx_head = (m_rx_head + 1U) % kTtyRxBufSize;
        --m_rx_count;
        return true;
    }

    [[nodiscard]] char translate_input_char(char chr, bool& skip_char) const noexcept {
        skip_char = false;
        if (chr == '\r') {
            if ((m_termios.c_iflag & Kernel::UAPI::IGNCR) != 0U) {
                skip_char = true;
                return chr;
            }
            if ((m_termios.c_iflag & Kernel::UAPI::ICRNL) != 0U) {
                return '\n';
            }
        } else if (chr == '\n' && (m_termios.c_iflag & Kernel::UAPI::INLCR) != 0U) {
            return '\r';
        }
        return chr;
    }

    linux_ktermios m_termios{default_termios()};
    uint32_t m_foreground_pgid{1U};
    uint8_t m_rx_buf[kTtyRxBufSize]{};
    size_t m_rx_head{0U};
    size_t m_rx_tail{0U};
    size_t m_rx_count{0U};
};

Superblock* g_devfs_sb = nullptr;
Inode* g_console_inode = nullptr;
uint64_t g_next_devfs_ino = 10ULL;
ConsoleTty g_console_tty{};

int64_t console_read(File* file, void* buf, size_t count, uint64_t* /*offset*/) noexcept {
    return g_console_tty.read(file, buf, count);
}

int64_t console_write(File* /*file*/, const void* buf, size_t count,
                      uint64_t* /*offset*/) noexcept {
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }
    HAL::get_console().write(static_cast<const char*>(buf), count);
    return static_cast<int64_t>(count);
}

int64_t console_lseek(File* /*file*/, int64_t /*offset*/, SeekWhence /*whence*/) noexcept {
    return kEspipe;
}

int32_t console_ioctl(File* /*file*/, uint64_t request, uint64_t arg) noexcept {
    return g_console_tty.ioctl(request, arg);
}

int16_t console_poll(File* file, int16_t events) noexcept {
    if (file == nullptr) {
        return 0;
    }
    return g_console_tty.poll(file->get_flags() & kOpenAccessMask, events);
}

int64_t devnull_read(File* /*file*/, void* /*buf*/, size_t /*count*/,
                     uint64_t* /*offset*/) noexcept {
    return 0;
}

int64_t devnull_write(File* /*file*/, const void* /*buf*/, size_t count,
                      uint64_t* /*offset*/) noexcept {
    return static_cast<int64_t>(count);
}

int64_t devnull_lseek(File* /*file*/, int64_t /*offset*/, SeekWhence /*whence*/) noexcept {
    return 0;
}

int32_t devfs_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || file->get_dentry() == nullptr || out_entry == nullptr) {
        return kEinval;
    }
    const uint64_t target_idx = file->get_offset();
    uint64_t idx = 0U;
    for (Dentry* child = file->get_dentry()->get_first_child(); child != nullptr;
         child = child->get_next_sibling()) {
        Inode* const child_inode = child->get_inode();
        if (!child->is_active() || child_inode == nullptr) {
            continue;
        }
        if (idx == target_idx) {
            out_entry->ino = child_inode->get_ino();
            out_entry->type = child_inode->get_mode() & kModeTypeMask;
            const StringView child_name = child->name_view();
            for (size_t name_idx = 0U; name_idx <= kMaxNameLen; ++name_idx) {
                out_entry->name[name_idx] =
                    (name_idx < child_name.size()) ? child_name[name_idx] : '\0';
            }
            file->set_offset(target_idx + 1U);
            return kOk;
        }
        ++idx;
    }
    return kEnoent;
}

const FileOperations kDevfsDirFileOps{
    .read = nullptr,
    .write = nullptr,
    .open = nullptr,
    .release = nullptr,
    .lseek = nullptr,
    .ioctl = nullptr,
    .fsync = nullptr,
    .readdir = devfs_readdir,
    .poll = nullptr,
};

const FileOperations kConsoleFileOps{
    .read = console_read,
    .write = console_write,
    .open = nullptr,
    .release = nullptr,
    .lseek = console_lseek,
    .ioctl = console_ioctl,
    .fsync = nullptr,
    .readdir = nullptr,
    .poll = console_poll,
};

const FileOperations kDevNullFileOps{
    .read = devnull_read,
    .write = devnull_write,
    .open = nullptr,
    .release = nullptr,
    .lseek = devnull_lseek,
    .ioctl = nullptr,
    .fsync = nullptr,
    .readdir = nullptr,
    .poll = nullptr,
};

} // namespace

void devfs_reset() noexcept {
    g_devfs_sb = nullptr;
    g_console_inode = nullptr;
    g_next_devfs_ino = 10ULL;
    console_reset_tty();
}

void console_reset_tty() noexcept {
    g_console_tty.reset();
}

int32_t devfs_register_chrdev(StringView name, uint32_t mode, uint64_t rdev,
                              const FileOperations* f_ops) noexcept {
    if (g_devfs_sb == nullptr || g_devfs_sb->get_root() == nullptr || f_ops == nullptr) {
        return kEinval;
    }
    if (name.empty() || name.size() > kMaxNameLen) {
        return kEinval;
    }
    Dentry* const root = g_devfs_sb->get_root();
    if (root->find_child(name) != nullptr) {
        return kEexist;
    }

    auto inode = RefPtr<Inode>::adopt(vfs_alloc_inode(g_devfs_sb));
    if (!inode) {
        return kEnomem;
    }
    inode->set_ino(g_next_devfs_ino++);
    inode->set_mode(kModeChr | (mode & 07777U));
    inode->set_rdev(rdev);
    inode->set_f_ops(f_ops);

    auto dentry = RefPtr<Dentry>::adopt(vfs_alloc_dentry(g_devfs_sb, root, name));
    if (!dentry) {
        return kEnomem;
    }
    Inode* const raw_inode = inode.detach();
    dentry->bind_inode(raw_inode);
    root->attach_child(dentry.detach());

    if (name == "console") {
        g_console_inode = raw_inode;
    }
    return kOk;
}

int32_t devfs_init_and_mount() noexcept {
    if (g_devfs_sb != nullptr) {
        return kEbusy;
    }

    Superblock* superblock = vfs_alloc_superblock();
    if (superblock == nullptr) {
        return kEnomem;
    }
    superblock->set_fs_type("devfs");
    superblock->set_block_size(4096U);
    superblock->set_flags(0U);

    auto root_inode = RefPtr<Inode>::adopt(vfs_alloc_inode(superblock));
    if (!root_inode) {
        vfs_free_superblock(superblock);
        return kEnomem;
    }
    root_inode->set_ino(1ULL);
    root_inode->set_mode(kModeDir | 0755U);
    root_inode->set_nlink(2U);
    root_inode->set_f_ops(&kDevfsDirFileOps);

    auto root_dentry = RefPtr<Dentry>::adopt(vfs_alloc_dentry(superblock, nullptr, "dev"));
    if (!root_dentry) {
        vfs_free_superblock(superblock);
        return kEnomem;
    }
    root_dentry->bind_inode(root_inode.detach());
    superblock->set_root(root_dentry.get());

    const int32_t mount_res = vfs_mount("/dev", superblock);
    if (mount_res != kOk) {
        vfs_free_superblock(superblock);
        return mount_res;
    }

    (void)root_dentry.detach();
    g_devfs_sb = superblock;

    int32_t reg_res = devfs_register_chrdev("console", 0666U, 0x0501ULL, &kConsoleFileOps);
    if (reg_res != kOk) {
        return reg_res;
    }
    reg_res = devfs_register_chrdev("tty", 0666U, 0x0500ULL, &kConsoleFileOps);
    if (reg_res != kOk) {
        return reg_res;
    }
    return devfs_register_chrdev("null", 0666U, 0x0103ULL, &kDevNullFileOps);
}

int32_t devfs_open_console(uint32_t flags, File** out_file) noexcept {
    if (out_file == nullptr) {
        return kEinval;
    }
    *out_file = nullptr;

    if (g_devfs_sb != nullptr) {
        const int32_t open_res = vfs_open("/dev/console", flags, 0U, out_file);
        if (open_res == kOk) {
            return kOk;
        }
    }

    // Fallback if devfs has not been mounted yet (e.g., isolated unit test).
    if (g_console_inode == nullptr) {
        Inode* fallback_ino = vfs_alloc_inode(nullptr);
        if (fallback_ino == nullptr) {
            return kEnomem;
        }
        fallback_ino->set_ino(2ULL);
        fallback_ino->set_mode(kModeChr | 0666U);
        fallback_ino->set_rdev(0x0501ULL);
        fallback_ino->set_f_ops(&kConsoleFileOps);
        g_console_inode = fallback_ino;
    }

    auto file = RefPtr<File>::adopt(vfs_alloc_file());
    if (!file) {
        return kEnfile;
    }
    g_console_inode->retain();
    file->set_inode(g_console_inode);
    file->set_flags(flags);
    file->set_f_ops(&kConsoleFileOps);
    *out_file = file.detach();
    return kOk;
}

void console_poll_rx() noexcept {
    g_console_tty.poll_rx();
}

void console_set_foreground_pgid(uint32_t pgid) noexcept {
    g_console_tty.set_foreground_pgid(pgid);
}

uint32_t console_get_foreground_pgid() noexcept {
    return g_console_tty.foreground_pgid();
}

} // namespace Kernel::FS
