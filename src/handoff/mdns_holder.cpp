/**
 * @file src/handoff/mdns_holder.cpp
 * @brief Implementation of `src/handoff/mdns_holder.h` (wave B).
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <utility>

// local includes
#include "mdns_holder.h"
#include "src/globals.h"
#include "src/logging.h"

#ifdef __linux__
namespace handoff {
  namespace {
    /**
     * @brief Registry of the live holders, so a scheduled rename check never touches a destroyed holder.
     *
     * The check takes `registry_mutex()` and keeps it while it runs; the destructor removes its holder under the same
     * mutex, after that no check can reach it. Lock order: registry mutex, then the holder's mutex. Each holder has a
     * unique id, so a check armed for a destroyed holder never matches a new holder at the same address.
     */
    std::mutex &registry_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    /**
     * @brief The live holders and their ids. Guarded by `registry_mutex()`.
     *
     * @return The map from holder to id.
     */
    std::map<const mdns_holder_t *, std::uint64_t> &registry() {
      static std::map<const mdns_holder_t *, std::uint64_t> map;
      return map;
    }

    /**
     * @brief Hand out a holder id.
     *
     * @return A process-unique, non-zero id.
     */
    std::uint64_t next_holder_id() {
      static std::atomic<std::uint64_t> counter {0};
      return ++counter;
    }

    /**
     * @brief Production settle wait.
     *
     * @param duration Time to sleep.
     */
    void default_wait(std::chrono::milliseconds duration) {
      std::this_thread::sleep_for(duration);
    }

    /**
     * @brief Production scheduler: run the task once on the task pool after the delay.
     *
     * @param task Task to run.
     * @param delay Delay before it runs.
     */
    void default_schedule(std::function<void()> task, std::chrono::milliseconds delay) {
      (void) task_pool.pushDelayed(std::move(task), delay);
    }
  }  // namespace

  mdns_holder_t::mdns_holder_t():
      mdns_holder_t(mdns_ops_t {}, mdns_timing_t {}) {
  }

  mdns_holder_t::mdns_holder_t(const mdns_ops_t &ops, const mdns_timing_t &timing):
      ops_ {ops},
      timing_ {timing} {
    if (!ops_.publish) {
      ops_.publish = &platf::publish::start;
    }
    if (!ops_.renamed) {
      ops_.renamed = &platf::publish::renamed;
    }
    if (!ops_.wait) {
      ops_.wait = &default_wait;
    }
    if (!ops_.schedule) {
      ops_.schedule = &default_schedule;
    }

    const std::scoped_lock lock {registry_mutex()};
    registry()[this] = next_holder_id();
  }

  mdns_holder_t::~mdns_holder_t() {
    stop();

    // After this no scheduled check can reach the holder (a running one holds the registry mutex).
    const std::scoped_lock lock {registry_mutex()};
    registry().erase(this);
  }

  bool mdns_holder_t::start(bool arm_rename_checks) {
    // Look the id up before taking `mutex_`: the lock order is registry mutex, then `mutex_`.
    std::uint64_t id = 0;
    if (arm_rename_checks) {
      const std::scoped_lock registry_lock {registry_mutex()};
      const auto it = registry().find(this);
      if (it != registry().end()) {
        id = it->second;
      }
    }

    const std::scoped_lock lock {mutex_};

    if (publisher_) {
      return true;
    }

    auto publisher = ops_.publish();
    if (!publisher) {
      BOOST_LOG(warning) << "mDNS is unavailable, the service is not published";
      return false;
    }

    publisher_ = std::move(publisher);
    ++epoch_;

    if (arm_rename_checks) {
      // The task runs on another thread; it never runs inline in `schedule`.
      const auto make_task = [this, id, epoch = epoch_]() {
        return [this, id, epoch]() {
          // Registry mutex first: it keeps the holder alive while the check runs.
          const std::scoped_lock registry_lock {registry_mutex()};
          const auto it = registry().find(this);
          if (it == registry().end() || it->second != id) {
            return;
          }

          const std::scoped_lock lock {mutex_};
          if (epoch_ != epoch || !publisher_) {
            return;
          }
          if (!ops_.renamed()) {
            return;
          }

          BOOST_LOG(info) << "mDNS name was changed by a collision, rebuilding";

          publisher_.reset();
          ++rebuilds_;
          publisher_ = ops_.publish();
          if (!publisher_) {
            BOOST_LOG(warning) << "mDNS rebuild failed, the service is not published";
          }
        };
      };

      ops_.schedule(make_task(), timing_.rename_first);
      ops_.schedule(make_task(), timing_.rename_second);
    }

    return true;
  }

  void mdns_holder_t::withdraw(std::chrono::milliseconds timeout) {
    const std::scoped_lock lock {mutex_};

    ++epoch_;

    if (publisher_) {
      publisher_.reset();
      ops_.wait(std::min(timeout, timing_.settle));
    }
  }

  void mdns_holder_t::stop() {
    const std::scoped_lock lock {mutex_};

    ++epoch_;
    publisher_.reset();
  }

  bool mdns_holder_t::published() const {
    const std::scoped_lock lock {mutex_};
    return publisher_ != nullptr;
  }

  std::uint32_t mdns_holder_t::rename_rebuilds() const {
    const std::scoped_lock lock {mutex_};
    return rebuilds_;
  }

  mdns_holder_t &mdns_holder() {
    // Leaked on purpose: shutdown calls `stop()` explicitly, and no destructor may race the exit handlers.
    static mdns_holder_t *holder = new mdns_holder_t();
    return *holder;
  }
}  // namespace handoff
#endif
