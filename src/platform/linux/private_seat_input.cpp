/** @file src/platform/linux/private_seat_input.cpp
 * @brief Private-seat input context with a closed capture qualification gate.
 */
#include "private_seat_input.h"

#include "src/config.h"
#include "src/logging.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <libvirtualhid/linux_key_mapping.hpp>
#include <limits>
#include <linux/input-event-codes.h>
#include <moonlight-common-c/src/Limelight.h>

namespace platf::private_seat_input {
  namespace {
    /** @brief Allocate process-monotonic producer generations without wrapping. */
    std::uint64_t next_generation() {
      static std::atomic<std::uint64_t> next {1};
      auto value = next.load(std::memory_order_relaxed);
      while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
          return value;
        }
      }
      return value;
    }

    /** @brief Reject reused or regressing capture generations process-wide. */
    bool claim_capture_generation(std::uint64_t generation) {
      static std::atomic<std::uint64_t> high_water {0};
      auto current = high_water.load(std::memory_order_relaxed);
      while (generation > current) {
        if (high_water.compare_exchange_weak(current, generation, std::memory_order_relaxed)) {
          return true;
        }
      }
      return false;
    }

    /** @brief Check membership in the shared virtual-key mapping. */
    bool supported_key(std::uint16_t code) {
      for (unsigned vk = 0; vk <= 255; ++vk) {
        if (lvh::key_code_to_linux(static_cast<lvh::KeyboardKeyCode>(vk)) == code) {
          return true;
        }
      }
      return false;
    }
  }  // namespace

  bool key_tracker::change(std::uint16_t code, bool down, const sink &emit) {
    auto it = refs_.find(code);
    if (down) {
      if (it == refs_.end()) {
        if (!emit(code, true)) {
          return false;
        }
        refs_.emplace(code, 1);
      } else {
        ++it->second;
      }
    } else {
      if (it == refs_.end()) {
        return false;
      }
      if (it->second == 1) {
        if (!emit(code, false)) {
          return false;
        }
        refs_.erase(it);
      } else {
        --it->second;
      }
    }
    return true;
  }

  bool key_tracker::down(std::uint64_t contributor, std::uint16_t code, const std::vector<std::uint16_t> &modifiers, const sink &emit) {
    if (!code || !emit || held(contributor)) {
      return false;
    }
    for (auto modifier : modifiers) {
      if (!modifier || !change(modifier, true, emit)) {
        reset();
        return false;
      }
    }
    if (!change(code, true, emit)) {
      reset();
      return false;
    }
    contributors_.emplace(contributor, code);
    for (auto modifier : modifiers) {
      if (!change(modifier, false, emit)) {
        reset();
        return false;
      }
    }
    return true;
  }

  bool key_tracker::up(std::uint64_t contributor, const sink &emit) {
    auto it = contributors_.find(contributor);
    if (it == contributors_.end() || !emit) {
      return false;
    }
    if (!change(it->second, false, emit)) {
      reset();
      return false;
    }
    contributors_.erase(it);
    return true;
  }

  void key_tracker::reset() {
    contributors_.clear();
    refs_.clear();
  }

  bool key_tracker::held(std::uint64_t contributor) const {
    return contributors_.contains(contributor);
  }

  std::uint64_t allocate_capture_generation() {
    static std::atomic<std::uint64_t> last {0};
    auto value = last.load(std::memory_order_relaxed);
    do {
      if (value == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
      }
    } while (!last.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value + 1;
  }

  void capture_gate::invalidate() {
    std::lock_guard lock {mutex};
    valid = false;
    latest.reset();
  }

  context::context():
      generation_ {next_generation()} {}

  bool context::begin_transition(welcome_callback completion) {
    reset();
    welcome_callback_ = std::move(completion);
    if (generation() == std::numeric_limits<std::uint64_t>::max() || !config::input.lanternbox_compositor_uid) {
      if (welcome_callback_) {
        auto callback = std::move(welcome_callback_);
        callback(false, generation(), {}, {});
      }
      return false;
    }
    client_ = std::make_unique<private_seat::client>(config::input.lanternbox_input_socket, *config::input.lanternbox_compositor_uid, supported_key, private_seat::client::clock::now());
    if (client_->status() == private_seat::state::closed) {
      if (welcome_callback_) {
        auto callback = std::move(welcome_callback_);
        callback(false, generation(), {}, {});
      }
      return false;
    }
    if (pump_) {
      pump_(weak_from_this(), generation());
    }
    return true;
  }

  bool context::drive(short revents, private_seat::client::clock::time_point now) {
    if (!client_) {
      return false;
    }
    // The socket is nonblocking and drive has a fixed per-turn byte budget.
    // Serialize flushing already-queued events with synchronous invalidation.
    auto gate = gate_;
    bool driven = false;
    if (gate) {
      std::lock_guard lock {gate->mutex};
      driven = gate->valid && client_->drive(revents, now);
    } else {
      driven = client_->drive(revents, now);
    }
    if (!driven) {
      if (!client_->error().empty()) {
        BOOST_LOG(warning) << "Private-seat transport retired: " << client_->error();
      }
      reset();
      return false;
    }
    if (reset_complete_ && welcome_cleanup_ack() && welcome_callback_) {
      auto callback = std::move(welcome_callback_);
      callback(true, generation(), client_->announcement(), client_->peer());
    }
    return true;
  }

  int context::fd() const {
    return client_ ? client_->fd() : -1;
  }

  short context::poll_events() const {
    return client_ ? client_->poll_events() : 0;
  }

  bool context::welcome_cleanup_ack() const {
    return reset_complete_ && client_ && client_->status() == private_seat::state::ready_to_send;
  }

  const private_seat::welcome *context::announcement() const {
    return welcome_cleanup_ack() ? &client_->announcement() : nullptr;
  }

  private_seat::peer_credentials context::peer() const {
    return client_ ? client_->peer() : private_seat::peer_credentials {};
  }

  bool context::publish_qualified_mapping(const capture_proof &p) {
    if (!reset_complete_ || !welcome_cleanup_ack() || mapping_ || p.producer_generation != generation() || !p.sole_full_normal_origin || !p.post_snapshot_frame_published || !p.capture_generation || !p.stream_width || !p.stream_height || !p.snapshot_sequence || p.frame_request_sequence <= p.snapshot_sequence || p.latest_published_sequence != p.frame_request_sequence) {
      return false;
    }
    const auto &w = client_->announcement();
    const auto actual_peer = client_->peer();
    if (p.wayland_peer.pid <= 0 || p.wayland_peer.pid != actual_peer.pid || p.wayland_peer.uid != actual_peer.uid || p.output_id != w.identity.output_id || p.epoch != w.identity.epoch || p.physical_width != w.physical_width || p.physical_height != w.physical_height || p.logical_width != w.logical_width || p.logical_height != w.logical_height || p.frame_output_id != p.output_id || p.frame_epoch != p.epoch || p.frame_capture_generation != p.capture_generation || p.frame_width != p.physical_width || p.frame_height != p.physical_height) {
      return false;
    }
    if (!claim_capture_generation(p.capture_generation)) {
      return false;
    }
    mapping_ = p;
    return true;
  }

  bool context::qualify_capture(const std::shared_ptr<capture_gate> &gate) {
    if (!gate || (gate_ && gate_ != gate) || mapping_) {
      return false;
    }
    bool failed_ready = false;
    {
      std::lock_guard lock {gate->mutex};
      if (!gate->valid) {
        return false;
      }
      gate_ = gate;
      if (!gate->latest || !publish_qualified_mapping(*gate->latest)) {
        return false;
      }
      failed_ready = !ready();
    }
    if (failed_ready) {
      reset();
      return false;
    }
    return true;
  }

  bool context::ready() {
    if (!reset_complete_ || !mapping_ || !welcome_cleanup_ack()) {
      return false;
    }
    const private_seat::viewport viewport {0, 0, mapping_->physical_width, mapping_->physical_height};
    if (!client_->send_ready(mapping_->capture_generation, viewport, private_seat::client::clock::now())) {
      return false;
    }
    qualified_ = true;
    return true;
  }

  bool context::active() const {
    return qualified_ && client_ && client_->status() == private_seat::state::active;
  }

  void context::reset() {
    if (gate_) {
      gate_->invalidate();
      gate_.reset();
    }
    qualified_ = false;
    mapping_.reset();
    reset_complete_ = !reset_callback_;
    generation_.store(next_generation(), std::memory_order_release);
    client_.reset();
    if (welcome_callback_) {
      auto callback = std::move(welcome_callback_);
      callback(false, generation(), {}, {});
    }
    if (reset_callback_) {
      reset_callback_(generation());
    }
  }

  std::uint64_t context::generation() const {
    return generation_.load(std::memory_order_acquire);
  }

  void context::on_reset(std::function<void(std::uint64_t)> callback) {
    reset_callback_ = std::move(callback);
  }

  void context::reset_complete(std::uint64_t token) {
    if (generation() == token) {
      reset_complete_ = true;
    }
  }

  void context::set_dispatcher(std::function<bool(std::function<void()>)> enqueue, std::function<void(std::weak_ptr<context>, std::uint64_t)> pump) {
    {
      std::lock_guard lock {enqueue_mutex_};
      enqueue_ = std::move(enqueue);
    }
    pump_ = std::move(pump);
  }

  bool context::post(std::function<void(context &)> operation) {
    std::function<bool(std::function<void()>)> enqueue;
    {
      std::lock_guard lock {enqueue_mutex_};
      enqueue = enqueue_;
    }
    if (!enqueue || !operation) {
      return false;
    }
    auto weak = weak_from_this();
    const auto token = generation();
    return enqueue([weak, token, operation = std::move(operation)]() {
      if (auto owner = weak.lock(); owner && owner->generation() == token) {
        operation(*owner);
      }
    });
  }

  bool context::post_transition(std::function<void(context &)> operation) {
    std::function<bool(std::function<void()>)> enqueue;
    {
      std::lock_guard lock {enqueue_mutex_};
      enqueue = enqueue_;
    }
    if (!enqueue || !operation) {
      return false;
    }
    return enqueue([weak = weak_from_this(), operation = std::move(operation)]() {
      if (auto owner = weak.lock()) {
        operation(*owner);
      }
    });
  }

  bool context::absolute_packet(double x, double y, double pw, double ph) {
    auto gate = gate_;
    if (!gate) {
      return false;
    }
    std::unique_lock lock {gate->mutex};
    if (!gate->valid || !active() || !mapping_ || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(pw) || !std::isfinite(ph) || pw <= 0 || ph <= 0) {
      return false;
    }
    const auto &m = *mapping_;
    const double dw = m.physical_width, dh = m.physical_height, sw = m.stream_width, sh = m.stream_height;
    const double scale = std::min(sw / dw, sh / dh);
    if (!std::isfinite(scale) || scale <= 0) {
      return false;
    }
    const double a = std::clamp(x, 0.0, pw) * sw / pw;
    const double b = std::clamp(y, 0.0, ph) * sh / ph;
    private_seat::event e;
    e.type = private_seat::event_type::absolute;
    e.x = std::clamp((a - (sw - scale * dw) / 2) / (scale * dw), 0.0, 1.0);
    e.y = std::clamp((b - (sh - scale * dh) / 2) / (scale * dh), 0.0, 1.0);
    if (client_->send_event(e, private_seat::client::clock::now())) {
      return true;
    }
    lock.unlock();
    reset();
    return false;
  }

  bool context::send_key(std::uint16_t code, bool down) {
    auto gate = gate_;
    if (!gate) {
      return false;
    }
    std::unique_lock lock {gate->mutex};
    if (!gate->valid || !active()) {
      return false;
    }
    private_seat::event event;
    event.type = private_seat::event_type::key;
    event.code = code;
    event.down = down;
    if (client_->send_event(event, private_seat::client::clock::now())) {
      return true;
    }
    BOOST_LOG(warning) << "Private-seat transition rejected: transport_state=" << static_cast<unsigned>(client_->status())
                       << " supported_mapping=" << supported_key(code) << " reason=" << client_->error();
    lock.unlock();
    reset();
    return false;
  }

  void context::send(const private_seat::event &event) {
    auto gate = gate_;
    if (!gate) {
      return;
    }
    std::unique_lock lock {gate->mutex};
    if (!gate->valid || !active()) {
      return;
    }
    if (!client_->send_event(event, private_seat::client::clock::now())) {
      lock.unlock();
      reset();
    }
  }

  void context::move(int x, int y) {
    private_seat::event e;
    e.type = private_seat::event_type::relative;
    e.x = x;
    e.y = y;
    send(e);
  }

  void context::button(int button, bool release) {
    private_seat::event e;
    e.type = private_seat::event_type::button;
    e.down = !release;
    switch (button) {
      case BUTTON_LEFT:
        e.code = BTN_LEFT;
        break;
      case BUTTON_RIGHT:
        e.code = BTN_RIGHT;
        break;
      case BUTTON_MIDDLE:
        e.code = BTN_MIDDLE;
        break;
      case BUTTON_X1:
        e.code = BTN_SIDE;
        break;
      case BUTTON_X2:
        e.code = BTN_EXTRA;
        break;
      default:
        return;
    }
    send(e);
  }

  void context::axis(int amount, bool horizontal) {
    private_seat::event e;
    e.type = private_seat::event_type::axis;
    if (horizontal) {
      e.horizontal = amount;
    } else {
      e.vertical = amount;
    }
    send(e);
  }

  void context::key(std::uint16_t keycode, bool release, std::uint8_t) {
    int code = lvh::key_code_to_linux(static_cast<lvh::KeyboardKeyCode>(keycode));
    if (code <= 0 || code > 0xffff) {
      return;
    }
    private_seat::event e;
    e.type = private_seat::event_type::key;
    e.code = static_cast<std::uint16_t>(code);
    e.down = !release;
    send(e);
  }

  std::optional<capture_transition> begin_capture_transition(std::weak_ptr<context> weak, const std::shared_ptr<capture_gate> &old_gate) {
    if (old_gate) {
      old_gate->invalidate();
    }
    auto owner = weak.lock();
    if (!owner) {
      return std::nullopt;
    }

    struct transition_wait {
      std::mutex mutex;  ///< Serializes timeout and completion.
      bool expired = false;  ///< The caller has stopped waiting.
      std::promise<std::optional<capture_transition>> result;  ///< Shared with a potentially late callback.
    };

    auto state = std::make_shared<transition_wait>();
    auto result = state->result.get_future();
    if (!owner->post_transition([state](context &ctx) {
          {
            std::lock_guard lock {state->mutex};
            if (state->expired) {
              return;
            }
          }
          ctx.begin_transition([state, weak = ctx.weak_from_this()](bool ok, std::uint64_t generation, private_seat::welcome welcome, private_seat::peer_credentials peer) {
            bool expired;
            {
              std::lock_guard lock {state->mutex};
              expired = state->expired;
              if (!expired) {
                state->result.set_value(ok ? std::optional<capture_transition> {{generation, welcome, peer}} : std::nullopt);
              }
            }
            if (expired && ok) {
              if (auto current = weak.lock(); current && current->generation() == generation) {
                current->reset();
              }
            }
          });
        })) {
      return std::nullopt;
    }
    owner.reset();
    if (result.wait_for(std::chrono::seconds {6}) == std::future_status::ready) {
      return result.get();
    }
    {
      std::lock_guard lock {state->mutex};
      state->expired = true;
    }
    if (auto current = weak.lock()) {
      current->post([](context &ctx) {
        ctx.reset();
      });
    }
    return std::nullopt;
  }

  bool post_capture_qualification(std::weak_ptr<context> weak, std::shared_ptr<capture_gate> gate) {
    auto owner = weak.lock();
    return owner && gate && owner->post([gate = std::move(gate)](context &ctx) {
      ctx.qualify_capture(gate);
    });
  }
}  // namespace platf::private_seat_input
