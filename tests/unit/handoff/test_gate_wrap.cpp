#ifdef __linux__
  /**
   * @file tests/unit/handoff/test_gate_wrap.cpp
   * @brief Tests for the 503 gate wrappers in src/handoff/gate_wrap.h (spec step 5, substeps 2 and 3).
   *
   * Written from the header doc comments, src/handoff/gate.h and docs/drain-handoff.md only.
   */

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstddef>
  #include <functional>
  #include <map>
  #include <memory>
  #include <optional>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <Simple-Web-Server/status_code.hpp>
  #include <Simple-Web-Server/utility.hpp>

  // local includes
  #include "src/handoff/gate.h"
  #include "src/handoff/gate_wrap.h"

using namespace std::chrono_literals;

namespace {
  using handoff::gate_t;

  /**
   * @brief Fake response: provides what gate_wrap.h requires (`close_connection_after_response` and `write`), and
   * records what the wrapper did to it.
   */
  struct fake_response_t {
    bool close_connection_after_response {false};  ///< Written by the refusal path.
    int write_calls {0};  ///< Number of `write` calls.
    std::optional<SimpleWeb::StatusCode> status;  ///< Status code of the last `write`.
    SimpleWeb::CaseInsensitiveMultimap headers;  ///< Header fields of the last `write`.
    gate_t *probe {nullptr};  ///< When set, `write` samples `probe->in_flight()`.
    std::size_t in_flight_at_write {0};  ///< Value sampled by `write` through `probe`.

    /**
     * @brief Record a write.
     *
     * @param code Status code written.
     * @param fields Header fields written.
     */
    void write(SimpleWeb::StatusCode code, const SimpleWeb::CaseInsensitiveMultimap &fields) {
      ++write_calls;
      status = code;
      headers = fields;
      if (probe) {
        in_flight_at_write = probe->in_flight();
      }
    }
  };

  /**
   * @brief Fake request: the wrappers never read the request.
   */
  struct fake_request_t {};

  using resp_t = std::shared_ptr<fake_response_t>;
  using req_t = std::shared_ptr<fake_request_t>;
  using handler_t = std::function<void(resp_t, req_t)>;
  using method_map_t = std::map<std::string, handler_t>;
  using resource_map_t = std::map<std::string, method_map_t>;

  /**
   * @brief Create a fresh fake response.
   *
   * @return New response.
   */
  resp_t make_response() {
    return std::make_shared<fake_response_t>();
  }

  /**
   * @brief Create a fresh fake request.
   *
   * @return New request.
   */
  req_t make_request() {
    return std::make_shared<fake_request_t>();
  }

  /**
   * @brief Check that a response is the 503 refusal: close flag set, one write, status 503, only `Connection: close`.
   *
   * @param response Response to check.
   * @param context Label used in the assertion messages.
   */
  void expect_refused(const resp_t &response, const std::string &context) {
    EXPECT_TRUE(response->close_connection_after_response)
      << context << ": a refused request must set close_connection_after_response";
    EXPECT_EQ(response->write_calls, 1)
      << context << ": a refused request must be answered with exactly one write";
    if (response->status) {
      EXPECT_EQ(*response->status, SimpleWeb::StatusCode::server_error_service_unavailable)
        << context << ": a refused request must be answered with 503 Service Unavailable";
    } else {
      ADD_FAILURE() << context << ": a refused request must be answered with a status code";
    }
    EXPECT_EQ(response->headers.size(), 1u)
      << context << ": the refusal must carry exactly one header (Connection: close) and nothing else";
    const auto it = response->headers.find("Connection");
    EXPECT_TRUE(it != response->headers.end() && it->second == "close")
      << context << ": the refusal must carry the header `Connection: close`";
  }

  /**
   * @brief Check that a response was not touched by a refusal.
   *
   * @param response Response to check.
   * @param context Label used in the assertion messages.
   */
  void expect_not_refused(const resp_t &response, const std::string &context) {
    EXPECT_FALSE(response->close_connection_after_response)
      << context << ": an admitted request must not set close_connection_after_response";
    EXPECT_EQ(response->write_calls, 0)
      << context << ": an admitted request must not be answered by the wrapper";
  }

  /**
   * @brief Fixture: a local gate per test, and the process-wide route gate lowered before and after each test.
   */
  class HandoffGateWrapTest: public ::testing::Test {
  protected:
    void SetUp() override {
      handoff::route_gate().lower();
    }

    void TearDown() override {
      handoff::route_gate().lower();
    }

    gate_t gate;  ///< Gate passed explicitly to the wrappers.
  };
}  // namespace

TEST_F(HandoffGateWrapTest, CountedRunsHandlerWhenGateIsDown) {
  bool ran = false;
  auto wrapped = handoff::counted<resp_t, req_t>([&ran](resp_t, req_t) {
    ran = true;
  },
                                                 gate);

  auto response = make_response();
  wrapped(response, make_request());

  EXPECT_TRUE(ran) << "a counted handler must run when the gate is down";
  expect_not_refused(response, "gate down");
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be back to 0 after a counted handler returns";
}

TEST_F(HandoffGateWrapTest, CountedInFlightIsOneDuringHandlerAndZeroAfter) {
  std::size_t during = 99;
  auto wrapped = handoff::counted<resp_t, req_t>(
    [this, &during](resp_t, req_t) {
      during = gate.in_flight();
    },
    gate
  );

  wrapped(make_response(), make_request());

  EXPECT_EQ(during, 1u) << "in_flight() must be 1 while a counted handler runs";
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be 0 after a counted handler returns";
}

TEST_F(HandoffGateWrapTest, CountedRaisedGateAnswers503AndDoesNotCallHandler) {
  gate.raise();
  bool ran = false;
  auto wrapped = handoff::counted<resp_t, req_t>([&ran](resp_t, req_t) {
    ran = true;
  },
                                                 gate);

  auto response = make_response();
  wrapped(response, make_request());

  EXPECT_FALSE(ran) << "a counted handler must not run while the gate is raised";
  expect_refused(response, "counted, raised gate");
  EXPECT_EQ(gate.in_flight(), 0u) << "a refused counted request must leave in_flight() at 0";
}

TEST_F(HandoffGateWrapTest, CountedRefusalIsAnsweredAfterEntryIsBackedOut) {
  gate.raise();
  auto wrapped = handoff::counted<resp_t, req_t>([](resp_t, req_t) {
  },
                                                 gate);

  auto response = make_response();
  response->probe = &gate;
  wrapped(response, make_request());

  EXPECT_EQ(response->in_flight_at_write, 0u)
    << "a refused request must already have backed out its in-flight entry when the 503 is written";
}

TEST_F(HandoffGateWrapTest, GatedRunsHandlerWhenGateIsDownAndDoesNotCount) {
  bool ran = false;
  std::size_t during = 99;
  auto wrapped = handoff::gated<resp_t, req_t>(
    [this, &ran, &during](resp_t, req_t) {
      ran = true;
      during = gate.in_flight();
    },
    gate
  );

  auto response = make_response();
  wrapped(response, make_request());

  EXPECT_TRUE(ran) << "a gated handler must run when the gate is down";
  EXPECT_EQ(during, 0u) << "a gated (not counted) handler must not increment in_flight()";
  expect_not_refused(response, "gated, gate down");
}

TEST_F(HandoffGateWrapTest, GatedRaisedGateRefusesAndDoesNotCallHandler) {
  gate.raise();
  bool ran = false;
  auto wrapped = handoff::gated<resp_t, req_t>([&ran](resp_t, req_t) {
    ran = true;
  },
                                               gate);

  auto response = make_response();
  wrapped(response, make_request());

  EXPECT_FALSE(ran) << "a gated handler must not run while the gate is raised";
  expect_refused(response, "gated, raised gate");
  EXPECT_EQ(gate.in_flight(), 0u) << "a gated route must never touch in_flight()";
}

TEST_F(HandoffGateWrapTest, CountedHandlerExceptionPropagatesAndCounterReturnsToZero) {
  auto wrapped = handoff::counted<resp_t, req_t>(
    [](resp_t, req_t) {
      throw std::runtime_error("handler failure");
    },
    gate
  );

  EXPECT_THROW(wrapped(make_response(), make_request()), std::runtime_error)
    << "an exception from a counted handler must propagate unchanged to the caller";
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must return to 0 when a counted handler throws";
  EXPECT_TRUE(gate.wait_idle(100ms)) << "wait_idle must not block after a counted handler threw";
}

TEST_F(HandoffGateWrapTest, GatedHandlerExceptionPropagates) {
  auto wrapped = handoff::gated<resp_t, req_t>(
    [](resp_t, req_t) {
      throw std::runtime_error("handler failure");
    },
    gate
  );

  EXPECT_THROW(wrapped(make_response(), make_request()), std::runtime_error)
    << "an exception from a gated handler must propagate unchanged to the caller";
  EXPECT_EQ(gate.in_flight(), 0u) << "a gated handler that throws must leave in_flight() at 0";
}

TEST_F(HandoffGateWrapTest, CountedHandlerThatRaisesTheGateItselfCompletesThenLaterRequestsAreRefused) {
  std::size_t during = 99;
  int runs = 0;
  auto wrapped = handoff::counted<resp_t, req_t>(
    [this, &during, &runs](resp_t, req_t) {
      ++runs;
      during = gate.in_flight();
      gate.raise();
    },
    gate
  );

  auto first = make_response();
  wrapped(first, make_request());

  EXPECT_EQ(during, 1u) << "in_flight() must be 1 inside a counted handler, also when it raises the gate";
  EXPECT_EQ(runs, 1) << "the handler that raised the gate must still run to completion";
  expect_not_refused(first, "handler raised the gate itself");
  EXPECT_TRUE(gate.raised()) << "the gate must stay raised after the handler raised it";
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be 0 after the handler returns";

  auto second = make_response();
  wrapped(second, make_request());

  EXPECT_EQ(runs, 1) << "a later counted request must not run the handler while the gate is raised";
  expect_refused(second, "later request after the handler raised the gate");
}

TEST_F(HandoffGateWrapTest, WrapResourcesCountsStateChangingMethodsAndGatesTheOthers) {
  std::map<std::string, std::size_t> seen;  // method name -> in_flight() seen inside its handler
  resource_map_t resources;
  const std::vector<std::string> counted_methods {"POST", "PUT", "PATCH", "DELETE"};
  const std::vector<std::string> gated_methods {"GET", "HEAD", "OPTIONS"};
  for (const auto &method : counted_methods) {
    resources["^/api/test$"][method] = [this, method, &seen](resp_t, req_t) {
      seen[method] = gate.in_flight();
    };
  }
  for (const auto &method : gated_methods) {
    resources["^/api/test$"][method] = [this, method, &seen](resp_t, req_t) {
      seen[method] = gate.in_flight();
    };
  }

  handoff::wrap_resources<resp_t, req_t>(resources, gate);

  for (const auto &method : counted_methods) {
    seen.erase(method);
    auto response = make_response();
    resources["^/api/test$"][method](response, make_request());
    ASSERT_EQ(seen.count(method), 1u) << method << " handler must run while the gate is down";
    EXPECT_EQ(seen[method], 1u) << method << " is state-changing and must be counted: in_flight() must be 1 in its handler";
    expect_not_refused(response, method + " with gate down");
  }
  for (const auto &method : gated_methods) {
    seen.erase(method);
    auto response = make_response();
    resources["^/api/test$"][method](response, make_request());
    ASSERT_EQ(seen.count(method), 1u) << method << " handler must run while the gate is down";
    EXPECT_EQ(seen[method], 0u) << method << " is not state-changing and must not be counted: in_flight() must be 0 in its handler";
    expect_not_refused(response, method + " with gate down");
  }
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be 0 after all wrapped handlers returned";
}

TEST_F(HandoffGateWrapTest, WrapResourcesRefusesEveryMethodWhileRaised) {
  int runs = 0;
  resource_map_t resources;
  const std::vector<std::string> methods {"GET", "HEAD", "OPTIONS", "POST", "PUT", "PATCH", "DELETE"};
  for (const auto &method : methods) {
    resources["^/api/test$"][method] = [&runs](resp_t, req_t) {
      ++runs;
    };
  }
  handoff::wrap_resources<resp_t, req_t>(resources, gate);

  gate.raise();
  for (const auto &method : methods) {
    auto response = make_response();
    resources["^/api/test$"][method](response, make_request());
    expect_refused(response, method + " with gate raised");
  }
  EXPECT_EQ(runs, 0) << "no wrapped handler may run while the gate is raised";
  EXPECT_EQ(gate.in_flight(), 0u) << "refused requests must leave in_flight() at 0";
}

TEST_F(HandoffGateWrapTest, WrapResourcesTwiceCountsTwice) {
  std::size_t during = 99;
  int runs = 0;
  resource_map_t resources;
  resources["^/api/test$"]["POST"] = [this, &during, &runs](resp_t, req_t) {
    ++runs;
    during = gate.in_flight();
  };

  handoff::wrap_resources<resp_t, req_t>(resources, gate);
  handoff::wrap_resources<resp_t, req_t>(resources, gate);

  resources["^/api/test$"]["POST"](make_response(), make_request());

  EXPECT_EQ(runs, 1) << "the POST handler must run once per request even when wrapped twice";
  EXPECT_EQ(during, 2u) << "a POST wrapped twice must be counted twice: in_flight() must be 2 inside the handler";
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be 0 after a double-wrapped handler returns";
}

TEST_F(HandoffGateWrapTest, CountedWrappedTwiceCountsTwice) {
  std::size_t during = 99;
  int runs = 0;
  auto inner = handoff::counted<resp_t, req_t>(
    [this, &during, &runs](resp_t, req_t) {
      ++runs;
      during = gate.in_flight();
    },
    gate
  );
  auto outer = handoff::counted<resp_t, req_t>(inner, gate);

  outer(make_response(), make_request());

  EXPECT_EQ(runs, 1) << "the handler must run once per request even when counted is applied twice";
  EXPECT_EQ(during, 2u) << "counted over counted must count twice: in_flight() must be 2 inside the handler";
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be 0 after the double-wrapped handler returns";
}

TEST_F(HandoffGateWrapTest, CountedWithoutGateArgumentUsesRouteGate) {
  auto &route = handoff::route_gate();
  std::size_t during = 99;
  auto wrapped = handoff::counted<resp_t, req_t>([&during, &route](resp_t, req_t) {
    during = route.in_flight();
  });

  wrapped(make_response(), make_request());
  EXPECT_EQ(during, 1u) << "the default gate of counted must be route_gate(): in_flight() must be 1 in the handler";

  route.raise();
  auto response = make_response();
  wrapped(response, make_request());

  expect_refused(response, "default gate raised");
  EXPECT_EQ(route.in_flight(), 0u) << "a refused request on route_gate() must leave its in_flight() at 0";
  EXPECT_FALSE(gate.raised()) << "raising route_gate() must not affect a local gate";
}

TEST_F(HandoffGateWrapTest, GatedWithoutGateArgumentUsesRouteGate) {
  auto &route = handoff::route_gate();
  auto wrapped = handoff::gated<resp_t, req_t>([](resp_t, req_t) {
  });

  route.raise();
  auto response = make_response();
  wrapped(response, make_request());

  expect_refused(response, "default gate of gated raised");
}

TEST_F(HandoffGateWrapTest, ConcurrentCountedRequestsAreDrainedAfterRaise) {
  constexpr int workers = 8;
  std::atomic<bool> release {false};
  std::atomic<int> entered {0};
  std::atomic<int> finished {0};

  auto wrapped = handoff::counted<resp_t, req_t>(
    [&release, &entered, &finished](resp_t, req_t) {
      entered.fetch_add(1);
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!release.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
      finished.fetch_add(1);
    },
    gate
  );

  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (int i = 0; i < workers; ++i) {
    threads.emplace_back([&wrapped]() {
      wrapped(make_response(), make_request());
    });
  }

  // Bounded wait until every worker is inside its handler.
  const auto start_deadline = std::chrono::steady_clock::now() + 2s;
  while (entered.load() < workers && std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(entered.load(), workers) << "all concurrent counted requests must be admitted and inside the handler";
  EXPECT_EQ(gate.in_flight(), static_cast<std::size_t>(workers))
    << "in_flight() must equal the number of admitted requests still running";

  gate.raise();
  EXPECT_FALSE(gate.wait_idle(50ms)) << "wait_idle must time out while admitted handlers are still running";

  auto late = make_response();
  wrapped(late, make_request());
  expect_refused(late, "request arriving while the gate is raised and handlers are still running");
  EXPECT_EQ(entered.load(), workers) << "a refused request must not run the handler";

  release.store(true);
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(gate.wait_idle(1000ms)) << "wait_idle must return true once every admitted handler has returned";
  EXPECT_EQ(gate.in_flight(), 0u) << "in_flight() must be 0 once every admitted handler has returned";
  EXPECT_EQ(finished.load(), workers) << "every admitted handler must run to completion";
}
#endif
