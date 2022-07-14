// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Copied from chrome/browser/push_messaging/push_messaging_app_identifier.cc

#include "neva/app_runtime/browser/push_messaging/push_messaging_app_identifier.h"

#include <string.h>

#include "base/check_op.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "net/dns/dns_util.h"

constexpr char kPushMessagingAppIdentifierPrefix[] = "wp:";
constexpr char kInstanceIDGuidSuffix[] = "-V2";

const char kPushMessagingAppIdentifierMap[] =
    "gcm.push_messaging_application_id_map";

namespace {

// sizeof is strlen + 1 since it's null-terminated.
constexpr size_t kPrefixLength = sizeof(kPushMessagingAppIdentifierPrefix) - 1;
constexpr size_t kGuidSuffixLength = sizeof(kInstanceIDGuidSuffix) - 1;

// Ok to use '#' as separator since only the origin of the url is used.
constexpr char kPrefValueSeparator = '#';
constexpr size_t kGuidLength = 36;  // "%08X-%04X-%04X-%04X-%012llX"

std::string FromTimeToString(base::Time time) {
  DCHECK(!time.is_null());
  return base::NumberToString(time.ToDeltaSinceWindowsEpoch().InMilliseconds());
}

bool FromStringToTime(const std::string& time_string,
                      absl::optional<base::Time>* time) {
  DCHECK(!time_string.empty());
  int64_t milliseconds;
  if (base::StringToInt64(time_string, &milliseconds) && milliseconds > 0) {
    *time = absl::make_optional(base::Time::FromDeltaSinceWindowsEpoch(
        base::TimeDelta::FromMilliseconds(milliseconds)));
    return true;
  }
  return false;
}

std::string MakePrefValue(
    const GURL& origin,
    int64_t service_worker_registration_id,
    const absl::optional<base::Time>& expiration_time = absl::nullopt,
    const std::string& web_app_id = std::string()) {
  std::string result = origin.spec() + kPrefValueSeparator +
                       base::NumberToString(service_worker_registration_id);
  result += kPrefValueSeparator;
  if (expiration_time)
    result += FromTimeToString(*expiration_time);
  if (!web_app_id.empty())
    result += kPrefValueSeparator + web_app_id;
  return result;
}

bool DisassemblePrefValue(const std::string& pref_value,
                          GURL* origin,
                          int64_t* service_worker_registration_id,
                          absl::optional<base::Time>* expiration_time,
                          std::string* web_app_id) {
  std::vector<std::string> parts =
      base::SplitString(pref_value, std::string(1, kPrefValueSeparator),
                        base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

  if (parts.size() < 3 || parts.size() > 4)
    return false;

  if (!base::StringToInt64(parts[1], service_worker_registration_id))
    return false;

  *origin = GURL(parts[0]);
  if (!origin->is_valid())
    return false;

  if (!parts[2].empty() && !FromStringToTime(parts[2], expiration_time))
    return false;

  if (parts.size() == 4) {
    *web_app_id = parts[3];
    if (!net::IsValidDNSDomain(*web_app_id))
      return false;
  }

  return true;
}

}  // namespace

// static
void PushMessagingAppIdentifier::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  // TODO(johnme): If push becomes enabled in incognito, be careful that this
  // pref is read from the right profile, as prefs defined in a regular profile
  // are visible in the corresponding incognito profile unless overridden.
  // TODO(johnme): Make sure this pref doesn't get out of sync after crashes.
  registry->RegisterDictionaryPref(kPushMessagingAppIdentifierMap);
}

// static
bool PushMessagingAppIdentifier::UseInstanceID(const std::string& app_id) {
  return base::EndsWith(app_id, kInstanceIDGuidSuffix,
                        base::CompareCase::SENSITIVE);
}

// static
PushMessagingAppIdentifier PushMessagingAppIdentifier::Generate(
    const GURL& origin,
    int64_t service_worker_registration_id,
    const absl::optional<base::Time>& expiration_time,
    const std::string& web_app_id) {
  // All new push subscriptions use Instance ID tokens.
  return GenerateInternal(origin, service_worker_registration_id,
                          true /* use_instance_id */, expiration_time,
                          web_app_id);
}

// static
PushMessagingAppIdentifier PushMessagingAppIdentifier::LegacyGenerateForTesting(
    const GURL& origin,
    int64_t service_worker_registration_id,
    const absl::optional<base::Time>& expiration_time) {
  return GenerateInternal(origin, service_worker_registration_id,
                          false /* use_instance_id */, expiration_time);
}

// static
PushMessagingAppIdentifier PushMessagingAppIdentifier::GenerateInternal(
    const GURL& origin,
    int64_t service_worker_registration_id,
    bool use_instance_id,
    const absl::optional<base::Time>& expiration_time,
    const std::string& web_app_id) {
  // Use uppercase GUID for consistency with GUIDs Push has already sent to GCM.
  // Also allows detecting case mangling; see code commented "crbug.com/461867".
  std::string guid = base::ToUpperASCII(base::GenerateGUID());
  if (use_instance_id) {
    guid.replace(guid.size() - kGuidSuffixLength, kGuidSuffixLength,
                 kInstanceIDGuidSuffix);
  }
  CHECK(!guid.empty());
  std::string app_id = kPushMessagingAppIdentifierPrefix + origin.spec() +
                       kPrefValueSeparator + guid;

  PushMessagingAppIdentifier app_identifier(app_id, origin,
                                            service_worker_registration_id,
                                            expiration_time, web_app_id);
  app_identifier.DCheckValid();
  return app_identifier;
}

// static
PushMessagingAppIdentifier PushMessagingAppIdentifier::FindByAppId(
    PrefService* pref_service,
    const std::string& app_id) {
  if (!base::StartsWith(app_id, kPushMessagingAppIdentifierPrefix,
                        base::CompareCase::INSENSITIVE_ASCII)) {
    return PushMessagingAppIdentifier();
  }

  // Since we now know this is a Push Messaging app_id, check the case hasn't
  // been mangled (crbug.com/461867).
  DCHECK_EQ(kPushMessagingAppIdentifierPrefix, app_id.substr(0, kPrefixLength));
  DCHECK_GE(app_id.size(), kPrefixLength + kGuidLength);
  DCHECK_EQ(app_id.substr(app_id.size() - kGuidLength),
            base::ToUpperASCII(app_id.substr(app_id.size() - kGuidLength)));

  const base::DictionaryValue* map =
      pref_service->GetDictionary(kPushMessagingAppIdentifierMap);

  const std::string* map_value = map->FindStringKey(app_id);

  if (!map_value || map_value->empty())
    return PushMessagingAppIdentifier();

  GURL origin;
  int64_t service_worker_registration_id;
  absl::optional<base::Time> expiration_time;
  std::string web_app_id;
  // Try disassemble the pref value, return an invalid app identifier if the
  // pref value is corrupted
  if (!DisassemblePrefValue(*map_value, &origin,
                            &service_worker_registration_id, &expiration_time,
                            &web_app_id)) {
    NOTREACHED();
    return PushMessagingAppIdentifier();
  }

  PushMessagingAppIdentifier app_identifier(app_id, origin,
                                            service_worker_registration_id,
                                            expiration_time, web_app_id);
  app_identifier.DCheckValid();
  return app_identifier;
}

// static
PushMessagingAppIdentifier PushMessagingAppIdentifier::FindByServiceWorker(
    PrefService* pref_service,
    const GURL& origin,
    int64_t service_worker_registration_id) {
  const std::string base_pref_value =
      MakePrefValue(origin, service_worker_registration_id);

  LOG(INFO) << __func__ << " base_pref_value=" << base_pref_value;

  const base::DictionaryValue* map =
      pref_service->GetDictionary(kPushMessagingAppIdentifierMap);
  LOG(INFO) << __func__ << " map=" << map;
  for (auto it = base::DictionaryValue::Iterator(*map); !it.IsAtEnd();
       it.Advance()) {
    if (it.value().is_string() &&
        base::StartsWith(it.value().GetString(), base_pref_value,
                         base::CompareCase::SENSITIVE))
      return FindByAppId(pref_service, it.key());
  }
  return PushMessagingAppIdentifier();
}

// static
std::vector<PushMessagingAppIdentifier> PushMessagingAppIdentifier::GetAll(
    PrefService* pref_service) {
  std::vector<PushMessagingAppIdentifier> result;

  const base::DictionaryValue* map =
      pref_service->GetDictionary(kPushMessagingAppIdentifierMap);
  for (auto it = base::DictionaryValue::Iterator(*map); !it.IsAtEnd();
       it.Advance()) {
    result.push_back(FindByAppId(pref_service, it.key()));
  }

  return result;
}

// static
void PushMessagingAppIdentifier::DeleteAllFromPrefs(PrefService* pref_service) {
  DictionaryPrefUpdate update(pref_service, kPushMessagingAppIdentifierMap);
  base::DictionaryValue* map = update.Get();
  map->Clear();
}

// static
size_t PushMessagingAppIdentifier::GetCount(PrefService* pref_service) {
  return pref_service->GetDictionary(kPushMessagingAppIdentifierMap)
      ->DictSize();
}

PushMessagingAppIdentifier::PushMessagingAppIdentifier(
    const PushMessagingAppIdentifier& other) = default;

PushMessagingAppIdentifier::PushMessagingAppIdentifier()
    : origin_(GURL::EmptyGURL()), service_worker_registration_id_(-1) {}

PushMessagingAppIdentifier::PushMessagingAppIdentifier(
    const std::string& app_id,
    const GURL& origin,
    int64_t service_worker_registration_id,
    const absl::optional<base::Time>& expiration_time,
    const std::string& web_app_id)
    : app_id_(app_id),
      origin_(origin),
      service_worker_registration_id_(service_worker_registration_id),
      expiration_time_(expiration_time),
      web_app_id_(web_app_id) {}

PushMessagingAppIdentifier::~PushMessagingAppIdentifier() {}

bool PushMessagingAppIdentifier::IsExpired() const {
  return (expiration_time_) ? *expiration_time_ < base::Time::Now() : false;
}

void PushMessagingAppIdentifier::PersistToPrefs(
    PrefService* pref_service) const {
  DCheckValid();

  DictionaryPrefUpdate update(pref_service, kPushMessagingAppIdentifierMap);
  base::DictionaryValue* map = update.Get();

  // Delete any stale entry with the same origin and Service Worker
  // registration id (hence we ensure there is a 1:1 not 1:many mapping).
  PushMessagingAppIdentifier old = FindByServiceWorker(
      pref_service, origin_, service_worker_registration_id_);
  if (!old.is_null())
    map->RemoveKey(old.app_id_);

  map->SetKey(app_id_, base::Value(MakePrefValue(
                           origin_, service_worker_registration_id_,
                           expiration_time_, web_app_id_)));
}

void PushMessagingAppIdentifier::DeleteFromPrefs(
    PrefService* pref_service) const {
  DCheckValid();

  DictionaryPrefUpdate update(pref_service, kPushMessagingAppIdentifierMap);
  base::DictionaryValue* map = update.Get();
  map->RemoveKey(app_id_);
}

void PushMessagingAppIdentifier::DCheckValid() const {
#if DCHECK_IS_ON()
  DCHECK_GE(service_worker_registration_id_, 0);

  DCHECK(origin_.is_valid());
  DCHECK_EQ(origin_.GetOrigin(), origin_);

  // "wp:"
  DCHECK_EQ(kPushMessagingAppIdentifierPrefix,
            app_id_.substr(0, kPrefixLength));

  // Optional (origin.spec() + '#')
  if (app_id_.size() != kPrefixLength + kGuidLength) {
    constexpr size_t suffix_length = 1 /* kPrefValueSeparator */ + kGuidLength;
    DCHECK_GT(app_id_.size(), kPrefixLength + suffix_length);
    DCHECK_EQ(origin_, GURL(app_id_.substr(
                           kPrefixLength,
                           app_id_.size() - kPrefixLength - suffix_length)));
    DCHECK_EQ(std::string(1, kPrefValueSeparator),
              app_id_.substr(app_id_.size() - suffix_length, 1));
  }

  // GUID. In order to distinguish them, an app_id created for an InstanceID
  // based subscription has the last few characters of the GUID overwritten with
  // kInstanceIDGuidSuffix (which contains non-hex characters invalid in GUIDs).
  std::string guid = app_id_.substr(app_id_.size() - kGuidLength);
  if (UseInstanceID(app_id_)) {
    DCHECK(!base::IsValidGUID(guid));

    // Replace suffix with valid hex so we can validate the rest of the string.
    guid = guid.replace(guid.size() - kGuidSuffixLength, kGuidSuffixLength,
                        kGuidSuffixLength, 'C');
  }
  DCHECK(base::IsValidGUID(guid));
#endif  // DCHECK_IS_ON()
}
