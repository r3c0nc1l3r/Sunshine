/**
 * @file tests/unit/handoff/test_app_digest.cpp
 * @brief Blind tests for the app digest (src/handoff/app_digest.*), written from the spec and header.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <string>
  #include <utility>
  #include <vector>

  // local includes
  #include "src/handoff/app_digest.h"

namespace {
  /**
   * @brief A non-trivial baseline input that every variant test changes in exactly one place.
   * @return Baseline digest input.
   */
  handoff::app_digest_input_t baseline() {
    handoff::app_digest_input_t input;
    input.prep = {
      {"do one", "undo one", false},
      {"do two", "undo two", true},
    };
    input.working_dir = "/opt/app";
    input.output = "/var/log/app.log";
    input.exit_timeout_s = 5;
    input.base_env = {
      {"PATH", "/usr/bin"},
      {"HOME", "/var/lib/app"},
    };
    return input;
  }

  /**
   * @brief Whether a string is exactly 64 lowercase hex characters.
   * @param text Candidate string.
   * @return `true` when it matches.
   */
  bool is_lower_hex_64(const std::string &text) {
    if (text.size() != 64) {
      return false;
    }
    for (const char c : text) {
      const bool digit = c >= '0' && c <= '9';
      const bool lower = c >= 'a' && c <= 'f';
      if (!digit && !lower) {
        return false;
      }
    }
    return true;
  }
}  // namespace

/**
 * @brief The digest is 64 lowercase hex characters.
 */
TEST(HandoffAppDigestTest, DigestIsSixtyFourLowercaseHexCharacters) {
  const auto digest = handoff::app_digest(baseline());
  EXPECT_TRUE(is_lower_hex_64(digest))
    << "app_digest must return exactly 64 lowercase hex characters, got '" << digest << "'.";
}

/**
 * @brief An empty input still gives a well-formed digest.
 */
TEST(HandoffAppDigestTest, EmptyInputGivesWellFormedDigest) {
  const auto digest = handoff::app_digest(handoff::app_digest_input_t {});
  EXPECT_TRUE(is_lower_hex_64(digest))
    << "An empty input must still produce 64 lowercase hex characters, got '" << digest << "'.";
}

/**
 * @brief The same input always gives the same digest.
 */
TEST(HandoffAppDigestTest, DigestIsDeterministic) {
  EXPECT_EQ(handoff::app_digest(baseline()), handoff::app_digest(baseline()))
    << "Two calls with equal inputs must return equal digests.";
}

/**
 * @brief Prep entry boundary: moving a byte from `do` to `undo` changes the digest.
 */
TEST(HandoffAppDigestTest, PrepDoUndoBoundaryIsLengthPrefixed) {
  handoff::app_digest_input_t left;
  left.prep = {{"a", "bc", false}};
  handoff::app_digest_input_t right;
  right.prep = {{"ab", "c", false}};
  EXPECT_NE(handoff::app_digest(left), handoff::app_digest(right))
    << "do=\"a\" undo=\"bc\" and do=\"ab\" undo=\"c\" must give different digests (length-prefix property).";
}

/**
 * @brief Environment boundary: moving a byte from the name to the value changes the digest.
 */
TEST(HandoffAppDigestTest, EnvNameValueBoundaryIsLengthPrefixed) {
  handoff::app_digest_input_t left;
  left.base_env = {{"a", "bc"}};
  handoff::app_digest_input_t right;
  right.base_env = {{"ab", "c"}};
  EXPECT_NE(handoff::app_digest(left), handoff::app_digest(right))
    << "env (\"a\",\"bc\") and (\"ab\",\"c\") must give different digests (length-prefix property).";
}

/**
 * @brief Working dir and output boundary: the split point between two strings is part of the digest.
 */
TEST(HandoffAppDigestTest, WorkingDirOutputBoundaryIsLengthPrefixed) {
  handoff::app_digest_input_t left;
  left.working_dir = "ab";
  left.output = "c";
  handoff::app_digest_input_t right;
  right.working_dir = "a";
  right.output = "bc";
  EXPECT_NE(handoff::app_digest(left), handoff::app_digest(right))
    << "working_dir and output split differently must give different digests.";
}

/**
 * @brief Name and value are not interchangeable in the environment.
 */
TEST(HandoffAppDigestTest, EnvNameAndValueSwapChangesDigest) {
  handoff::app_digest_input_t left;
  left.base_env = {{"x", "y"}};
  handoff::app_digest_input_t right;
  right.base_env = {{"y", "x"}};
  EXPECT_NE(handoff::app_digest(left), handoff::app_digest(right))
    << "Swapping an environment name and value must change the digest.";
}

/**
 * @brief The environment's order does not matter.
 */
TEST(HandoffAppDigestTest, EnvOrderIsIndependent) {
  auto forward = baseline();
  auto reversed = baseline();
  std::reverse(reversed.base_env.begin(), reversed.base_env.end());
  EXPECT_EQ(handoff::app_digest(forward), handoff::app_digest(reversed))
    << "The base environment is sorted by name, so its input order must not change the digest.";
}

/**
 * @brief Shuffled environment orders with more entries still give the same digest.
 */
TEST(HandoffAppDigestTest, EnvOrderIndependentWithManyEntries) {
  handoff::app_digest_input_t a;
  a.base_env = {{"ZED", "1"}, {"ALPHA", "2"}, {"MID", "3"}, {"BETA", "4"}};
  handoff::app_digest_input_t b;
  b.base_env = {{"BETA", "4"}, {"MID", "3"}, {"ZED", "1"}, {"ALPHA", "2"}};
  EXPECT_EQ(handoff::app_digest(a), handoff::app_digest(b))
    << "Any permutation of the environment must give the same digest.";
}

/**
 * @brief Variables named SUNSHINE_HANDOFF_* are excluded from the digest.
 */
TEST(HandoffAppDigestTest, SunshineHandoffVariablesAreExcluded) {
  auto with_handoff = baseline();
  with_handoff.base_env.emplace_back("SUNSHINE_HANDOFF_FDS", "3:4:5");
  with_handoff.base_env.emplace_back("SUNSHINE_HANDOFF_CHANNEL", "7");
  EXPECT_EQ(handoff::app_digest(with_handoff), handoff::app_digest(baseline()))
    << "Variables starting with SUNSHINE_HANDOFF_ must not change the digest.";
}

/**
 * @brief The value of a SUNSHINE_HANDOFF_* variable does not matter either.
 */
TEST(HandoffAppDigestTest, SunshineHandoffVariableValueDoesNotMatter) {
  auto first = baseline();
  first.base_env.emplace_back("SUNSHINE_HANDOFF_STATE", "one");
  auto second = baseline();
  second.base_env.emplace_back("SUNSHINE_HANDOFF_STATE", "two");
  EXPECT_EQ(handoff::app_digest(first), handoff::app_digest(second))
    << "Two different values of an excluded SUNSHINE_HANDOFF_* variable must give the same digest.";
}

/**
 * @brief Other SUNSHINE_ variables are not excluded; only the SUNSHINE_HANDOFF_ prefix is.
 */
TEST(HandoffAppDigestTest, OtherSunshineVariablesAreCounted) {
  auto with_client = baseline();
  with_client.base_env.emplace_back("SUNSHINE_CLIENT_WIDTH", "1920");
  EXPECT_NE(handoff::app_digest(with_client), handoff::app_digest(baseline()))
    << "A SUNSHINE_CLIENT_WIDTH variable must count in the digest; only SUNSHINE_HANDOFF_* is excluded.";
}

/**
 * @brief A name that merely contains SUNSHINE_HANDOFF_ without starting with it is counted.
 */
TEST(HandoffAppDigestTest, NameContainingHandoffPrefixElsewhereIsCounted) {
  auto with_name = baseline();
  with_name.base_env.emplace_back("NOT_SUNSHINE_HANDOFF_X", "1");
  EXPECT_NE(handoff::app_digest(with_name), handoff::app_digest(baseline()))
    << "Only names that start with SUNSHINE_HANDOFF_ are excluded.";
}

/**
 * @brief Changing the `do` command of a prep entry changes the digest.
 */
TEST(HandoffAppDigestTest, PrepDoCommandChangesDigest) {
  auto changed = baseline();
  changed.prep[0].do_cmd = "do one changed";
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing a prep entry's do command must change the digest.";
}

/**
 * @brief Changing the `undo` command of a prep entry changes the digest.
 */
TEST(HandoffAppDigestTest, PrepUndoCommandChangesDigest) {
  auto changed = baseline();
  changed.prep[1].undo_cmd = "undo two changed";
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing a prep entry's undo command must change the digest.";
}

/**
 * @brief Changing the `elevated` flag of a prep entry changes the digest.
 */
TEST(HandoffAppDigestTest, PrepElevatedFlagChangesDigest) {
  auto changed = baseline();
  changed.prep[0].elevated = true;
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing a prep entry's elevated flag must change the digest.";
}

/**
 * @brief Adding a prep entry changes the digest (the entry count is part of it).
 */
TEST(HandoffAppDigestTest, AddingPrepEntryChangesDigest) {
  auto changed = baseline();
  changed.prep.push_back({"do three", "undo three", false});
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Adding a prep entry must change the digest.";
}

/**
 * @brief Removing a prep entry changes the digest.
 */
TEST(HandoffAppDigestTest, RemovingPrepEntryChangesDigest) {
  auto changed = baseline();
  changed.prep.pop_back();
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Removing a prep entry must change the digest.";
}

/**
 * @brief The prep list is ordered, so swapping two entries changes the digest.
 */
TEST(HandoffAppDigestTest, PrepOrderChangesDigest) {
  auto swapped = baseline();
  std::swap(swapped.prep[0], swapped.prep[1]);
  EXPECT_NE(handoff::app_digest(swapped), handoff::app_digest(baseline()))
    << "The prep list is ordered; swapping two entries must change the digest.";
}

/**
 * @brief Changing the app's working directory changes the digest.
 */
TEST(HandoffAppDigestTest, WorkingDirChangesDigest) {
  auto changed = baseline();
  changed.working_dir = "/opt/other";
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing working-dir must change the digest.";
}

/**
 * @brief Changing the app's output file changes the digest.
 */
TEST(HandoffAppDigestTest, OutputChangesDigest) {
  auto changed = baseline();
  changed.output = "/var/log/other.log";
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing output must change the digest.";
}

/**
 * @brief Changing the exit timeout changes the digest.
 */
TEST(HandoffAppDigestTest, ExitTimeoutChangesDigest) {
  auto changed = baseline();
  changed.exit_timeout_s = 6;
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing exit-timeout must change the digest.";
}

/**
 * @brief Changing an environment variable's name changes the digest.
 */
TEST(HandoffAppDigestTest, EnvNameChangesDigest) {
  auto changed = baseline();
  changed.base_env[0].first = "PATH2";
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Renaming an environment variable must change the digest.";
}

/**
 * @brief Changing an environment variable's value changes the digest.
 */
TEST(HandoffAppDigestTest, EnvValueChangesDigest) {
  auto changed = baseline();
  changed.base_env[1].second = "/srv/other";
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Changing an environment variable's value must change the digest.";
}

/**
 * @brief Adding a (non-handoff) environment variable changes the digest through the entry count.
 */
TEST(HandoffAppDigestTest, EnvEntryCountChangesDigest) {
  auto changed = baseline();
  changed.base_env.emplace_back("EXTRA", "1");
  EXPECT_NE(handoff::app_digest(changed), handoff::app_digest(baseline()))
    << "Adding an environment variable must change the digest.";
}

/**
 * @brief An empty prep list and a prep list with one empty entry are different inputs.
 */
TEST(HandoffAppDigestTest, EmptyPrepDiffersFromOneEmptyEntry) {
  handoff::app_digest_input_t none;
  handoff::app_digest_input_t one_empty;
  one_empty.prep = {{"", "", false}};
  EXPECT_NE(handoff::app_digest(none), handoff::app_digest(one_empty))
    << "No prep entries and one empty prep entry must give different digests.";
}

/**
 * @brief Equal inputs built independently give the same digest (the digest depends on values only).
 */
TEST(HandoffAppDigestTest, EqualValuesGiveEqualDigest) {
  const auto first = handoff::app_digest(baseline());
  const auto second = handoff::app_digest(baseline());
  EXPECT_EQ(first, second) << "Independently built equal inputs must produce equal digests.";
}
#endif
