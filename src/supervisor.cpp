#include "procwatch/supervisor.hpp"

#include "procwatch/analyzer.hpp"
#include "procwatch/proc_reader.hpp"
#include "procwatch/sampler.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace procwatch
{

// Internal helpers used only by this source file
namespace
{

using Clock = std::chrono::steady_clock;

// How long the target may take to shut down after SIGTERM
constexpr std::chrono::seconds stop_grace_period{2};

// How long to wait for the target to disappear after SIGKILL
constexpr std::chrono::seconds kill_timeout{5};

// How long reads may keep failing while the target is still running
constexpr std::chrono::seconds read_failure_limit{5};

[[noreturn]] void throw_system_error(const std::string& message)
{
    throw std::system_error(errno, std::generic_category(), message);
}

bool is_stop_signal(int signal_number)
{
    return signal_number == SIGINT || signal_number == SIGTERM;
}

// Blocks SIGINT, SIGTERM and SIGCHLD so they can be received synchronously.
// This avoids signal handlers and cannot miss a signal between two checks
class SignalGuard
{
public:
    SignalGuard()
    {
        ::sigemptyset(&signals_);
        ::sigaddset(&signals_, SIGINT);
        ::sigaddset(&signals_, SIGTERM);
        ::sigaddset(&signals_, SIGCHLD);

        if (::sigprocmask(SIG_BLOCK, &signals_, &previous_mask_) != 0)
        {
            throw_system_error("Cannot block signals");
        }

        // An inherited SIG_IGN would make the kernel discard the exit status
        struct sigaction default_action{};
        default_action.sa_handler = SIG_DFL;
        ::sigemptyset(&default_action.sa_mask);

        if (::sigaction(SIGCHLD, &default_action, &previous_child_action_) != 0)
        {
            const int error = errno;
            ::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
            errno = error;
            throw_system_error("Cannot configure SIGCHLD");
        }
    }

    ~SignalGuard()
    {
        // Signals that arrived after the target was classified are dropped,
        // so they cannot interrupt writing the report
        sigset_t pending = signals_;

        for (const int signal_number : {SIGINT, SIGTERM, SIGCHLD})
        {
            if (::sigismember(&previous_mask_, signal_number) == 1)
            {
                ::sigdelset(&pending, signal_number);
            }
        }

        const timespec no_wait{};

        while (::sigtimedwait(&pending, nullptr, &no_wait) > 0 || errno == EINTR)
        {
        }

        ::sigaction(SIGCHLD, &previous_child_action_, nullptr);
        ::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;

    // Gives the child process the signal state procwatch was started with
    void restore_in_child() const noexcept
    {
        ::sigaction(SIGCHLD, &previous_child_action_, nullptr);
        ::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    // Waits for a signal or the given time.
    // Returns true if a stop signal was received
    bool wait_until(Clock::time_point wake_time) const
    {
        using std::chrono::duration_cast;
        using std::chrono::nanoseconds;

        const nanoseconds remaining = std::max(
            duration_cast<nanoseconds>(wake_time - Clock::now()),
            nanoseconds::zero()
        );
        const auto whole_seconds =
            duration_cast<std::chrono::seconds>(remaining);

        timespec timeout{};
        timeout.tv_sec = static_cast<std::time_t>(whole_seconds.count());
        timeout.tv_nsec = static_cast<long>((remaining - whole_seconds).count());

        const int received = ::sigtimedwait(&signals_, nullptr, &timeout);

        if (received < 0 && errno != EAGAIN && errno != EINTR)
        {
            throw_system_error("Cannot wait for signals");
        }

        return is_stop_signal(received);
    }

    // Returns true if a stop signal is already pending
    bool consume_stop_request() const
    {
        const timespec no_wait{};
        bool stop_requested = false;

        while (true)
        {
            const int received = ::sigtimedwait(&signals_, nullptr, &no_wait);

            if (received > 0)
            {
                stop_requested = stop_requested || is_stop_signal(received);
            }
            else if (errno != EINTR)
            {
                return stop_requested;
            }
        }
    }

private:
    sigset_t signals_{};
    sigset_t previous_mask_{};
    struct sigaction previous_child_action_{};
};

// Runs in the forked child and never returns. A failed exec is reported
// through the pipe, which closes by itself when exec succeeds
[[noreturn]] void exec_target(
    char* const argv[],
    int exec_pipe,
    const SignalGuard& signals
) noexcept
{
    signals.restore_in_child();

    ::execvp(argv[0], argv);

    const int error = errno;

    while (::write(exec_pipe, &error, sizeof(error)) < 0 && errno == EINTR)
    {
    }

    ::_exit(127);
}

// Owns the target process from its launch until it has been collected
class ChildProcess
{
public:
    // Starts the command and returns once exec has succeeded.
    // Throws LaunchError if exec fails
    ChildProcess(
        const std::vector<std::string>& command,
        const SignalGuard& signals
    )
    {
        // Prepared before fork, so the child does not need to allocate
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);

        for (const std::string& argument : command)
        {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }

        argv.push_back(nullptr);

        int exec_pipe[2];

        if (::pipe2(exec_pipe, O_CLOEXEC) != 0)
        {
            throw_system_error("Cannot create the launch pipe");
        }

        pid_ = ::fork();

        if (pid_ < 0)
        {
            const int error = errno;
            ::close(exec_pipe[0]);
            ::close(exec_pipe[1]);
            errno = error;
            throw_system_error("Cannot start the target process");
        }

        if (pid_ == 0)
        {
            exec_target(argv.data(), exec_pipe[1], signals);
        }

        ::close(exec_pipe[1]);

        // End of file means exec succeeded. Data is the errno of a failed exec
        int exec_error = 0;
        ssize_t count = 0;

        do
        {
            count = ::read(exec_pipe[0], &exec_error, sizeof(exec_error));
        } while (count < 0 && errno == EINTR);

        ::close(exec_pipe[0]);

        if (count == 0)
        {
            return;
        }

        // The destructor does not run when a constructor throws
        ::kill(pid_, SIGKILL);
        wait_for_exit();

        if (count == static_cast<ssize_t>(sizeof(exec_error)))
        {
            throw LaunchError(
                "cannot execute '" + command.front() + "': " +
                std::generic_category().message(exec_error)
            );
        }

        throw std::runtime_error("Cannot confirm that the target has started");
    }

    // Terminates and collects a target that is still running
    ~ChildProcess()
    {
        if (running_ && ::kill(pid_, SIGKILL) == 0)
        {
            wait_for_exit();
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    pid_t pid() const
    {
        return pid_;
    }

    // Returns the wait status once the target has ended
    std::optional<int> try_collect()
    {
        int status = 0;
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);

        if (result == pid_)
        {
            running_ = false;
            return status;
        }

        if (result < 0 && errno != EINTR)
        {
            running_ = false;
            throw_system_error("Cannot wait for the target process");
        }

        return std::nullopt;
    }

    void send_signal(int signal_number) const
    {
        if (::kill(pid_, signal_number) != 0 && errno != ESRCH)
        {
            throw_system_error("Cannot signal the target process");
        }
    }

    // Gives up on a process that the system cannot terminate
    void abandon()
    {
        running_ = false;
    }

private:
    void wait_for_exit() noexcept
    {
        int status = 0;

        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR)
        {
        }
    }

    pid_t pid_{-1};
    bool running_{true};
};

// Decides how the target ended. Only a stop request that has been observed
// by now makes the end intentional
ProcessResult classify_termination(int status, bool stop_requested)
{
    ProcessResult result;

    if (WIFEXITED(status))
    {
        if (stop_requested)
        {
            result.termination = Termination::StoppedByProcwatch;
        }
        else
        {
            result.termination = Termination::Exited;
            result.exit_code = WEXITSTATUS(status);
        }

        return result;
    }

    const int signal_number = WTERMSIG(status);

    if (stop_requested &&
        (is_stop_signal(signal_number) || signal_number == SIGKILL))
    {
        result.termination = Termination::StoppedByProcwatch;
    }
    else
    {
        result.termination = Termination::Signaled;
        result.signal = signal_number;
    }

    return result;
}

// Creates the finding for a target that exited with an error or was killed
// by a signal. A target stopped by procwatch produces no finding
std::optional<Finding> make_termination_finding(
    const ProcessResult& result,
    Clock::time_point timestamp
)
{
    std::string description;

    if (result.termination == Termination::Exited && result.exit_code != 0)
    {
        description =
            "Process exited with code " + std::to_string(result.exit_code);
    }
    else if (result.termination == Termination::Signaled)
    {
        description =
            "Process was killed by signal " + std::to_string(result.signal);
    }
    else
    {
        return std::nullopt;
    }

    return Finding{abnormal_termination_finding, description, timestamp};
}

// A read can fail briefly while the target exits, so only failures that
// continue for a while are treated as an error
void check_read_result(
    const ReadResult& result,
    Clock::time_point now,
    std::optional<Clock::time_point>& failing_since
)
{
    if (result.status == ReadStatus::Ok)
    {
        failing_since.reset();
        return;
    }

    if (!failing_since)
    {
        failing_since = now;
    }

    if (now - *failing_since >= read_failure_limit)
    {
        throw std::runtime_error(
            "Cannot read the metrics of the target process: " + result.error
        );
    }
}

// Samples the target until it has ended and returns how it ended
ProcessResult monitor_target(
    ChildProcess& child,
    Sampler& sampler,
    const SignalGuard& signals,
    std::chrono::milliseconds max_duration
)
{
    // The Analyzer starts the warm-up at the first sample as well
    const Clock::time_point first_sample_time = Clock::now();
    const Clock::time_point deadline = first_sample_time + max_duration;

    std::optional<Clock::time_point> failing_since;
    check_read_result(
        sampler.sample(first_sample_time), first_sample_time, failing_since);

    bool stop_requested = false;
    std::optional<Clock::time_point> kill_time;
    std::optional<Clock::time_point> give_up_time;

    while (true)
    {
        if (const std::optional<int> status = child.try_collect())
        {
            // A stop signal may have arrived together with the target's end
            stop_requested = signals.consume_stop_request() || stop_requested;
            return classify_termination(*status, stop_requested);
        }

        const Clock::time_point now = Clock::now();

        if (!stop_requested)
        {
            const Clock::time_point next_sample = sampler.next_sample_time();

            // A sample due at the deadline is taken before stopping
            if (next_sample <= deadline && next_sample <= now)
            {
                check_read_result(sampler.sample(now), now, failing_since);
                continue;
            }

            stop_requested = now >= deadline;
        }

        if (stop_requested && !kill_time)
        {
            child.send_signal(SIGTERM);
            kill_time = now + stop_grace_period;
        }

        if (kill_time && !give_up_time && now >= *kill_time)
        {
            child.send_signal(SIGKILL);
            give_up_time = now + kill_timeout;
        }

        if (give_up_time && now >= *give_up_time)
        {
            child.abandon();
            throw std::runtime_error("The target process did not terminate");
        }

        Clock::time_point wake_time =
            std::min(sampler.next_sample_time(), deadline);

        if (kill_time)
        {
            wake_time = give_up_time ? *give_up_time : *kill_time;
        }

        stop_requested = signals.wait_until(wake_time) || stop_requested;
    }
}

} // namespace

Supervisor::Supervisor(const Config& config)
    : config_(config)
{
}

// Launches the target, monitors it and prepares the report data
ReportData Supervisor::run()
{
    // Declared first, so signals stay blocked until the target is collected
    const SignalGuard signals;

    // Created before the launch, so these failures never leave a target behind
    const Analyzer analyzer(config_);
    ProcReader reader;

    ReportData data;
    data.started_at = std::chrono::system_clock::now();
    data.start_time = Clock::now();

    ChildProcess child(config_.command, signals);
    Sampler sampler(child.pid(), std::move(reader), config_.sampling_interval);

    data.process_result =
        monitor_target(child, sampler, signals, config_.max_duration);
    data.end_time = Clock::now();
    data.samples = sampler.samples();

    data.findings = analyzer.analyze(data.samples);

    // The target ended after the last sample, so the order stays chronological
    if (auto finding =
            make_termination_finding(data.process_result, data.end_time))
    {
        data.findings.push_back(std::move(*finding));
    }

    data.stable = data.findings.empty();

    return data;
}

}