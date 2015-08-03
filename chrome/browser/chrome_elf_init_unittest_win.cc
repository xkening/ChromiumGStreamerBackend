// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chrome_elf_init_win.h"

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/metrics/field_trial.h"
#include "base/strings/string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/test_reg_util_win.h"
#include "chrome_elf/chrome_elf_constants.h"
#include "components/variations/entropy_provider.h"
#include "components/variations/variations_associated_data.h"
#include "components/version_info/version_info.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "version.h"  // NOLINT

namespace {

const char kBrowserBlacklistTrialEnabledGroupName[] = "Enabled";

class ChromeBlacklistTrialTest : public testing::Test {
 protected:
  ChromeBlacklistTrialTest() {}
  ~ChromeBlacklistTrialTest() override {}

  void SetUp() override {
    testing::Test::SetUp();

    override_manager_.OverrideRegistry(HKEY_CURRENT_USER);

    blacklist_registry_key_.reset(
        new base::win::RegKey(HKEY_CURRENT_USER,
                              blacklist::kRegistryBeaconPath,
                              KEY_QUERY_VALUE | KEY_SET_VALUE));
  }

  DWORD GetBlacklistState() {
    DWORD blacklist_state = blacklist::BLACKLIST_STATE_MAX;
    blacklist_registry_key_->ReadValueDW(blacklist::kBeaconState,
                                         &blacklist_state);

    return blacklist_state;
  }

  base::string16 GetBlacklistVersion() {
    base::string16 blacklist_version;
    blacklist_registry_key_->ReadValue(blacklist::kBeaconVersion,
                                       &blacklist_version);

    return blacklist_version;
  }

  scoped_ptr<base::win::RegKey> blacklist_registry_key_;
  registry_util::RegistryOverrideManager override_manager_;
  content::TestBrowserThreadBundle test_browser_thread_bundle_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ChromeBlacklistTrialTest);
};

// Ensure that the default trial sets up the blacklist beacons.
TEST_F(ChromeBlacklistTrialTest, DefaultRun) {
  // Set some dummy values as beacons.
  blacklist_registry_key_->WriteValue(blacklist::kBeaconState,
                                      blacklist::BLACKLIST_DISABLED);
  blacklist_registry_key_->WriteValue(blacklist::kBeaconVersion, L"Data");

  // This setup code should result in the default group, which should have
  // the blacklist set up.
  InitializeChromeElf();

  // Ensure the beacon values are now correct, indicating the
  // blacklist beacon was setup.
  ASSERT_EQ(blacklist::BLACKLIST_ENABLED, GetBlacklistState());
  base::string16 version(base::UTF8ToUTF16(version_info::GetVersionNumber()));
  ASSERT_EQ(version, GetBlacklistVersion());
}

// Ensure that the blacklist is disabled for any users in the
// "BlacklistDisabled" finch group.
TEST_F(ChromeBlacklistTrialTest, BlacklistDisabledRun) {
  // Set the beacons to enabled values.
  blacklist_registry_key_->WriteValue(blacklist::kBeaconState,
                                      blacklist::BLACKLIST_ENABLED);
  blacklist_registry_key_->WriteValue(blacklist::kBeaconVersion, L"Data");

  // Create the field trial with the blacklist disabled group.
  base::FieldTrialList field_trial_list(
    new metrics::SHA1EntropyProvider("test"));

  scoped_refptr<base::FieldTrial> trial(
    base::FieldTrialList::CreateFieldTrial(
      kBrowserBlacklistTrialName, kBrowserBlacklistTrialDisabledGroupName));

  // This setup code should now delete any existing blacklist beacons.
  InitializeChromeElf();

  // Ensure invalid values are returned to indicate that the beacon
  // values are indeed gone.
  ASSERT_EQ(blacklist::BLACKLIST_STATE_MAX, GetBlacklistState());
  ASSERT_EQ(base::string16(), GetBlacklistVersion());
}

TEST_F(ChromeBlacklistTrialTest, VerifyFirstRun) {
  BrowserBlacklistBeaconSetup();

  // Verify the state is properly set after the first run.
  ASSERT_EQ(blacklist::BLACKLIST_ENABLED, GetBlacklistState());

  base::string16 version(base::UTF8ToUTF16(version_info::GetVersionNumber()));
  ASSERT_EQ(version, GetBlacklistVersion());
}

TEST_F(ChromeBlacklistTrialTest, BlacklistFailed) {
  // Ensure when the blacklist set up failed we set the state to disabled for
  // future runs.
  blacklist_registry_key_->WriteValue(blacklist::kBeaconVersion,
                                      TEXT(CHROME_VERSION_STRING));
  blacklist_registry_key_->WriteValue(blacklist::kBeaconState,
                                      blacklist::BLACKLIST_SETUP_FAILED);

  BrowserBlacklistBeaconSetup();

  ASSERT_EQ(blacklist::BLACKLIST_DISABLED, GetBlacklistState());
}

TEST_F(ChromeBlacklistTrialTest, VersionChanged) {
  // Mark the blacklist as disabled for an older version, it should
  // get enabled for this new version.  Also record a non-zero number of
  // setup failures, which should be reset to zero.
  blacklist_registry_key_->WriteValue(blacklist::kBeaconVersion,
                                      L"old_version");
  blacklist_registry_key_->WriteValue(blacklist::kBeaconState,
                                      blacklist::BLACKLIST_DISABLED);
  blacklist_registry_key_->WriteValue(blacklist::kBeaconAttemptCount,
                                      blacklist::kBeaconMaxAttempts);

  BrowserBlacklistBeaconSetup();

  // The beacon should now be marked as enabled for the current version.
  ASSERT_EQ(blacklist::BLACKLIST_ENABLED, GetBlacklistState());

  base::string16 expected_version(
      base::UTF8ToUTF16(version_info::GetVersionNumber()));
  ASSERT_EQ(expected_version, GetBlacklistVersion());

  // The counter should be reset.
  DWORD attempt_count = blacklist::kBeaconMaxAttempts;
  blacklist_registry_key_->ReadValueDW(blacklist::kBeaconAttemptCount,
                                       &attempt_count);
  ASSERT_EQ(static_cast<DWORD>(0), attempt_count);
}

TEST_F(ChromeBlacklistTrialTest, AddFinchBlacklistToRegistry) {
  // Create the field trial with the blacklist enabled group.
  base::FieldTrialList field_trial_list(
      new metrics::SHA1EntropyProvider("test"));

  scoped_refptr<base::FieldTrial> trial(base::FieldTrialList::CreateFieldTrial(
      kBrowserBlacklistTrialName, kBrowserBlacklistTrialEnabledGroupName));

  // Set up the trial with the desired parameters.
  std::map<std::string, std::string> desired_params;
  desired_params["TestDllName1"] = "TestDll1.dll";
  desired_params["TestDllName2"] = "TestDll2.dll";

  variations::AssociateVariationParams(
      kBrowserBlacklistTrialName,
      kBrowserBlacklistTrialEnabledGroupName,
      desired_params);

  // This should add the dlls in those parameters to the registry.
  AddFinchBlacklistToRegistry();

  // Check that all the values in desired_params were added to the registry.
  base::win::RegKey finch_blacklist_registry_key(
      HKEY_CURRENT_USER,
      blacklist::kRegistryFinchListPath,
      KEY_QUERY_VALUE | KEY_SET_VALUE);

  ASSERT_EQ(desired_params.size(),
            finch_blacklist_registry_key.GetValueCount());

  for (std::map<std::string, std::string>::iterator it = desired_params.begin();
       it != desired_params.end();
       ++it) {
    std::wstring name = base::UTF8ToWide(it->first);
    ASSERT_TRUE(finch_blacklist_registry_key.HasValue(name.c_str()));
  }
}

}  // namespace
