/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStoreClientInternal.h"

#include <openr/common/OpenrClient.h>
#include <openr/common/Util.h>

#include <folly/SharedMutex.h>
#include <folly/String.h>

namespace openr {

KvStoreClientInternal::KvStoreClientInternal(
    OpenrEventBase* eventBase,
    std::string const& nodeId,
    KvStore* kvStore,
    std::optional<std::chrono::milliseconds> checkPersistKeyPeriod)
    : nodeId_(nodeId),
      eventBase_(eventBase),
      kvStore_(kvStore),
      checkPersistKeyPeriod_(checkPersistKeyPeriod) {
  // sanity check
  CHECK_NE(eventBase_, static_cast<void*>(nullptr));
  CHECK(!nodeId.empty());
  CHECK(kvStore_);

  // Fiber to process thrift::Publication from KvStore
  taskFuture_ = eventBase_->addFiberTaskFuture([
    q = std::move(kvStore_->getKvStoreUpdatesReader()),
    this
  ]() mutable noexcept {
    LOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybePublication = q.get(); // perform read
      VLOG(2) << "Received KvStore update";
      if (maybePublication.hasError()) {
        LOG(INFO) << "Terminating KvStore updates processing fiber";
        break;
      }
      processPublication(maybePublication.value());
    }
  });

  // initialize timers
  initTimers();
}

KvStoreClientInternal::~KvStoreClientInternal() {
  // - If EventBase is stopped or it is within the evb thread, run immediately;
  // - Otherwise, will wait the EventBase to run;
  eventBase_->getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // destory your timers
    advertiseKeyValsTimer_.reset();
    ttlTimer_.reset();
    checkPersistKeyTimer_.reset();
  });

  // Stop kvstore internal if not stopped yet
  stop();
}

void
KvStoreClientInternal::stop() {
  // wait for fiber to be closed before destroy KvStoreClientInternal
  taskFuture_.cancel();
  taskFuture_.wait();
  LOG(INFO) << "Fiber task closed...";
}

void
KvStoreClientInternal::initTimers() {
  // Create timer to advertise pending key-vals
  advertiseKeyValsTimer_ =
      folly::AsyncTimeout::make(*eventBase_->getEvb(), [this]() noexcept {
        VLOG(3) << "Received timeout event.";

        // Advertise all pending keys
        advertisePendingKeys();

        // Clear all backoff if they are passed away
        for (auto& [_, areaBackoffs] : backoffs_) {
          for (auto& [key, backoff] : areaBackoffs) {
            if (backoff.canTryNow()) {
              VLOG(2) << "Clearing off the exponential backoff for key " << key;
              backoff.reportSuccess();
            }
          }
        }
      });

  // Create ttl timer
  ttlTimer_ = folly::AsyncTimeout::make(
      *eventBase_->getEvb(), [this]() noexcept { advertiseTtlUpdates(); });

  // Create check persistKey timer
  if (checkPersistKeyPeriod_.has_value()) {
    checkPersistKeyTimer_ = folly::AsyncTimeout::make(
        *eventBase_->getEvb(), [this]() noexcept { checkPersistKeyInStore(); });
    checkPersistKeyTimer_->scheduleTimeout(checkPersistKeyPeriod_.value());
  }
}

void
KvStoreClientInternal::checkPersistKeyInStore() {
  std::chrono::milliseconds timeout{checkPersistKeyPeriod_.value()};

  // go through persisted keys map for each area
  for (const auto& persistKeyValsEntry : persistedKeyVals_) {
    const auto& area = persistKeyValsEntry.first;
    auto& persistedKeyVals = persistKeyValsEntry.second;

    if (persistedKeyVals.empty()) {
      continue;
    }

    thrift::KeyGetParams params;
    thrift::Publication pub;
    for (auto const& key : persistedKeyVals) {
      params.keys_ref()->emplace_back(key.first);
    }

    // Get KvStore response
    try {
      pub = *(kvStore_->getKvStoreKeyVals(area, params).get());
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to get keyvals from kvstore. Exception: "
                 << ex.what();
      // retry in 1 sec
      timeout = 1000ms;
      checkPersistKeyTimer_->scheduleTimeout(1000ms);
      continue;
    }

    // Find expired keys from latest KvStore
    std::unordered_map<std::string, thrift::Value> keyVals;
    for (auto const& key : persistedKeyVals) {
      auto rxkey = pub.keyVals_ref()->find(key.first);
      if (rxkey == pub.keyVals_ref()->end()) {
        keyVals.emplace(key.first, persistedKeyVals.at(key.first));
      }
    }

    // Advertise to KvStore
    if (not keyVals.empty()) {
      const auto ret = setKeysHelper(area, std::move(keyVals));
      if (!ret.has_value()) {
        LOG(ERROR) << "Error sending SET_KEY request to KvStore.";
      }
    }
    processPublication(pub);
  }

  timeout = std::min(timeout, checkPersistKeyPeriod_.value());
  checkPersistKeyTimer_->scheduleTimeout(timeout);
}

bool
KvStoreClientInternal::persistKey(
    AreaId const& area,
    std::string const& key,
    std::string const& value,
    std::chrono::milliseconds const ttl /* = Constants::kTtlInfInterval */) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  VLOG(3) << "KvStoreClientInternal: persistKey called for key:" << key
          << " area:" << area.t;

  auto& persistedKeyVals = persistedKeyVals_[area];
  const auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  auto& keysToAdvertise = keysToAdvertise_[area];
  auto& callbacks = keyCallbacks_[area];

  // Look it up in the existing
  auto keyIt = persistedKeyVals.find(key);

  // Default thrift value to use with invalid version=0
  thrift::Value thriftValue = createThriftValue(
      0,
      nodeId_,
      value,
      ttl.count(),
      0 /* ttl version */,
      std::nullopt /* hash */);
  CHECK(thriftValue.value_ref());

  // Retrieve the existing value for the key. If key is persisted before then
  // it is the one we have cached locally else we need to fetch it from KvStore
  if (keyIt == persistedKeyVals.end()) {
    // Get latest value from KvStore
    auto maybeValue = getKey(area, key);
    if (maybeValue.has_value()) {
      thriftValue = maybeValue.value();
      // TTL update pub is never saved in kvstore
      DCHECK(thriftValue.value_ref());
    }
  } else {
    thriftValue = keyIt->second;
    if (thriftValue.value_ref().value() == value and
        *thriftValue.ttl_ref() == ttl.count()) {
      // this is a no op, return early and change no state
      return false;
    }
    auto ttlIt = keyTtlBackoffs.find(key);
    if (ttlIt != keyTtlBackoffs.end()) {
      thriftValue.ttlVersion_ref() = *ttlIt->second.first.ttlVersion_ref();
    }
  }

  // Decide if we need to re-advertise the key back to kv-store
  bool valueChange = false;
  if (!(*thriftValue.version_ref())) {
    thriftValue.version_ref() = 1;
    valueChange = true;
  } else if (
      *thriftValue.originatorId_ref() != nodeId_ ||
      *thriftValue.value_ref() != value) {
    (*thriftValue.version_ref())++;
    thriftValue.ttlVersion_ref() = 0;
    thriftValue.value_ref() = value;
    *thriftValue.originatorId_ref() = nodeId_;
    valueChange = true;
  }

  // We must update ttl value to new one. When ttl changes but value doesn't
  // then we should advertise ttl immediately so that new ttl is in effect
  const bool hasTtlChanged = ttl.count() != *thriftValue.ttl_ref();
  thriftValue.ttl_ref() = ttl.count();

  // Cache it in persistedKeyVals_. Override the existing one
  persistedKeyVals[key] = thriftValue;

  // Override existing backoff as well
  backoffs_[area][key] = ExponentialBackoff<std::chrono::milliseconds>(
      Constants::kInitialBackoff, Constants::kMaxBackoff);

  // Invoke callback with updated value
  auto cb = callbacks.find(key);
  if (cb != callbacks.end() && valueChange) {
    (cb->second)(key, thriftValue);
  }

  // Add keys to list of pending keys
  if (valueChange) {
    keysToAdvertise.insert(key);
  }

  // Best effort to advertise pending keys
  advertisePendingKeys();

  scheduleTtlUpdates(
      area,
      key,
      *thriftValue.version_ref(),
      *thriftValue.ttlVersion_ref(),
      ttl.count(),
      hasTtlChanged);

  return true;
}

thrift::Value
KvStoreClientInternal::buildThriftValue(
    AreaId const& area,
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */) {
  // Create 'thrift::Value' object which will be sent to KvStore
  thrift::Value thriftValue = createThriftValue(
      version, nodeId_, value, ttl.count(), 0 /* ttl version */, 0 /* hash */);
  CHECK(thriftValue.value_ref());

  // Use one version number higher than currently in KvStore if not specified
  if (!version) {
    auto maybeValue = getKey(area, key);
    if (maybeValue.has_value()) {
      thriftValue.version_ref() = *maybeValue->version_ref() + 1;
    } else {
      thriftValue.version_ref() = 1;
    }
  }
  return thriftValue;
}

std::optional<folly::Unit>
KvStoreClientInternal::setKey(
    AreaId const& area,
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */) {
  return setKey(area, key, buildThriftValue(area, key, value, version, ttl));
}

std::optional<folly::Unit>
KvStoreClientInternal::setKey(
    AreaId const& area,
    std::string const& key,
    thrift::Value const& thriftValue) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());
  CHECK(thriftValue.value_ref());

  VLOG(3) << "KvStoreClientInternal: setKey called for key " << key;

  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, thriftValue);

  const auto ret = setKeysHelper(area, std::move(keyVals));

  scheduleTtlUpdates(
      area,
      key,
      *thriftValue.version_ref(),
      *thriftValue.ttlVersion_ref(),
      *thriftValue.ttl_ref(),
      false /* advertiseImmediately */);

  return ret;
}

void
KvStoreClientInternal::scheduleTtlUpdates(
    AreaId const& area,
    std::string const& key,
    uint32_t version,
    uint32_t ttlVersion,
    int64_t ttl,
    bool advertiseImmediately) {
  // infinite TTL does not need update

  auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  if (ttl == Constants::kTtlInfinity) {
    // in case ttl is finite before
    keyTtlBackoffs.erase(key);
    return;
  }

  // do not send value to reduce update overhead
  thrift::Value ttlThriftValue = createThriftValue(
      version,
      nodeId_,
      std::string("") /* value */,
      ttl,
      ttlVersion /* ttl version */,
      0 /* hash */);
  ttlThriftValue.value_ref().reset();
  CHECK(not ttlThriftValue.value_ref().has_value());

  // renew before Ttl expires about every ttl/3, i.e., try twice
  // use ExponentialBackoff to track remaining time
  keyTtlBackoffs[key] = std::make_pair(
      ttlThriftValue,
      ExponentialBackoff<std::chrono::milliseconds>(
          std::chrono::milliseconds(ttl / 4),
          std::chrono::milliseconds(ttl / 4 + 1)));

  // Delay first ttl advertisement by (ttl / 4). We have just advertised key or
  // update and would like to avoid sending unncessary immediate ttl update
  if (not advertiseImmediately) {
    keyTtlBackoffs.at(key).second.reportError();
  }

  advertiseTtlUpdates();
}

void
KvStoreClientInternal::unsetKey(AreaId const& area, std::string const& key) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  VLOG(3) << "KvStoreClientInternal: unsetKey called for key " << key
          << " area " << area.t;

  persistedKeyVals_[area].erase(key);
  backoffs_[area].erase(key);
  keyTtlBackoffs_[area].erase(key);
  keysToAdvertise_[area].erase(key);
}

void
KvStoreClientInternal::clearKey(
    AreaId const& area,
    std::string const& key,
    std::string keyValue,
    std::chrono::milliseconds ttl) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  VLOG(1) << "KvStoreClientInternal: clear key called for key " << key;

  // erase keys
  unsetKey(area, key);

  // if key doesn't exist in KvStore no need to add it as "empty". This
  // condition should not exist.
  auto maybeValue = getKey(area, key);
  if (!maybeValue.has_value()) {
    return;
  }
  // overwrite all values, increment version, reset value to empty
  auto& thriftValue = maybeValue.value();
  *thriftValue.originatorId_ref() = nodeId_;
  (*thriftValue.version_ref())++;
  thriftValue.ttl_ref() = ttl.count();
  thriftValue.ttlVersion_ref() = 0;
  thriftValue.value_ref() = std::move(keyValue);

  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, std::move(thriftValue));
  // Advertise to KvStore
  const auto ret = setKeysHelper(area, std::move(keyVals));
  if (!ret.has_value()) {
    LOG(ERROR) << "Error sending SET_KEY request to KvStore";
  }
}

std::optional<thrift::Value>
KvStoreClientInternal::getKey(AreaId const& area, std::string const& key) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());
  CHECK(kvStore_);

  VLOG(3) << "KvStoreClientInternal: getKey called for key " << key << ", area "
          << area.t;

  thrift::Publication pub;
  try {
    thrift::KeyGetParams params;
    params.keys_ref()->emplace_back(key);
    pub = *(kvStore_->getKvStoreKeyVals(area, params).get());
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to get keyvals from kvstore. Exception: "
               << ex.what();
    return std::nullopt;
  }
  VLOG(3) << "Received " << pub.keyVals_ref()->size() << " key-vals.";

  auto it = pub.keyVals_ref()->find(key);
  if (it == pub.keyVals_ref()->end()) {
    LOG(ERROR) << "Key: " << key << " NOT found in kvstore. Area: " << area.t;
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::unordered_map<std::string, thrift::Value>>
KvStoreClientInternal::dumpAllWithPrefix(
    AreaId const& area, const std::string& prefix /* = "" */) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());
  CHECK(kvStore_);

  thrift::Publication pub;
  try {
    thrift::KeyDumpParams params;
    *params.prefix_ref() = prefix;
    if (not prefix.empty()) {
      params.keys_ref() = {prefix};
    }
    pub = *kvStore_->dumpKvStoreKeys(std::move(params), {area}).get()->begin();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to add peers to kvstore. Exception: " << ex.what();
    return std::nullopt;
  }
  return *pub.keyVals_ref();
}

std::optional<thrift::Value>
KvStoreClientInternal::subscribeKey(
    AreaId const& area,
    std::string const& key,
    KeyCallback callback,
    bool fetchKeyValue) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());
  CHECK(bool(callback)) << "Callback function for " << key << " is empty";

  VLOG(3) << "KvStoreClientInternal: subscribeKey called for key " << key;
  keyCallbacks_[area][key] = std::move(callback);

  return fetchKeyValue ? getKey(area, key) : std::nullopt;
}

void
KvStoreClientInternal::subscribeKeyFilter(
    KvStoreFilters kvFilters, KeyCallback callback) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  keyPrefixFilter_ = std::move(kvFilters);
  keyPrefixFilterCallback_ = std::move(callback);
  return;
}

void
KvStoreClientInternal::unsubscribeKeyFilter() {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  keyPrefixFilterCallback_ = nullptr;
  keyPrefixFilter_ = KvStoreFilters({}, {});
  return;
}

void
KvStoreClientInternal::unsubscribeKey(
    AreaId const& area, std::string const& key) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  VLOG(3) << "KvStoreClientInternal: unsubscribeKey called for key " << key;
  // Store callback into KeyCallback map
  if (keyCallbacks_[area].erase(key) == 0) {
    LOG(WARNING) << "UnsubscribeKey called for non-existing key" << key;
  }
}

void
KvStoreClientInternal::setKvCallback(KeyCallback callback) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  kvCallback_ = std::move(callback);
}

void
KvStoreClientInternal::processExpiredKeys(
    thrift::Publication const& publication) {
  auto const& expiredKeys = *publication.expiredKeys_ref();

  // NOTE: default construct empty map if it didn't exist
  auto& callbacks = keyCallbacks_[AreaId{publication.get_area()}];
  for (auto const& key : expiredKeys) {
    /* callback registered by the thread */
    if (kvCallback_) {
      kvCallback_(key, std::nullopt);
    }
    /* key specific registered callback */
    auto cb = callbacks.find(key);
    if (cb != callbacks.end()) {
      (cb->second)(key, std::nullopt);
    }
  }
}

void
KvStoreClientInternal::processPublication(
    thrift::Publication const& publication) {
  // Go through received key-values and find out the ones which need update
  CHECK(not publication.area_ref()->empty());
  AreaId area{publication.get_area()};
  LOG(INFO) << "Area: " << area.t;
  // NOTE: default construct empty containers if they didn't exist
  auto& persistedKeyVals = persistedKeyVals_[area];
  auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  auto& keysToAdvertise = keysToAdvertise_[area];
  auto& callbacks = keyCallbacks_[area];

  for (auto const& kv : *publication.keyVals_ref()) {
    auto const& key = kv.first;
    auto const& rcvdValue = kv.second;
    LOG(INFO) << "Key: " << key;
    if (not rcvdValue.value_ref()) {
      // ignore TTL update
      continue;
    }

    if (kvCallback_) {
      kvCallback_(key, rcvdValue);
    }

    // Update local keyVals as per need
    auto it = persistedKeyVals.find(key);
    auto cb = callbacks.find(key);
    // set key w/ finite TTL
    auto sk = keyTtlBackoffs.find(key);

    // key set but not persisted
    if (sk != keyTtlBackoffs.end() and it == persistedKeyVals.end()) {
      auto& setValue = sk->second.first;
      if (*rcvdValue.version_ref() > *setValue.version_ref() or
          (*rcvdValue.version_ref() == *setValue.version_ref() and
           *rcvdValue.originatorId_ref() > *setValue.originatorId_ref())) {
        // key lost, cancel TTL update
        keyTtlBackoffs.erase(sk);
      } else if (
          *rcvdValue.version_ref() == *setValue.version_ref() and
          *rcvdValue.originatorId_ref() == *setValue.originatorId_ref() and
          *rcvdValue.ttlVersion_ref() > *setValue.ttlVersion_ref()) {
        // If version, value and originatorId is same then we should look up
        // ttlVersion and update local value if rcvd ttlVersion is higher
        // NOTE: We don't need to advertise the value back
        if (sk != keyTtlBackoffs.end() and
            *sk->second.first.ttlVersion_ref() < *rcvdValue.ttlVersion_ref()) {
          VLOG(1) << "Bumping TTL version for (key, version, originatorId) "
                  << folly::sformat(
                         "({}, {}, {})",
                         key,
                         *rcvdValue.version_ref(),
                         *rcvdValue.originatorId_ref())
                  << " to " << (*rcvdValue.ttlVersion_ref() + 1) << " from "
                  << *setValue.ttlVersion_ref();
          setValue.ttlVersion_ref() = *rcvdValue.ttlVersion_ref() + 1;
        }
      }
    }

    if (it == persistedKeyVals.end()) {
      // We need to alert callback if a key is not persisted and we
      // received a change notification for it.
      if (cb != callbacks.end()) {
        (cb->second)(key, rcvdValue);
      }
      // callback for a given key filter
      if (keyPrefixFilterCallback_ &&
          keyPrefixFilter_.keyMatch(key, rcvdValue)) {
        keyPrefixFilterCallback_(key, rcvdValue);
      }
      // Skip rest of the processing. We are not interested.
      continue;
    }

    // Ignore if received version is strictly old
    auto& currentValue = it->second;
    if (*currentValue.version_ref() > *rcvdValue.version_ref()) {
      continue;
    }

    // Update if our version is old
    bool valueChange = false;
    if (*currentValue.version_ref() < *rcvdValue.version_ref()) {
      // Bump-up version number
      *currentValue.originatorId_ref() = nodeId_;
      currentValue.version_ref() = *rcvdValue.version_ref() + 1;
      currentValue.ttlVersion_ref() = 0;
      valueChange = true;
    }

    // version is same but originator id is different. Then we need to
    // advertise with a higher version.
    if (!valueChange and *rcvdValue.originatorId_ref() != nodeId_) {
      *currentValue.originatorId_ref() = nodeId_;
      (*currentValue.version_ref())++;
      currentValue.ttlVersion_ref() = 0;
      valueChange = true;
    }

    // Need to re-advertise if value doesn't matches. This can happen when our
    // update is reflected back
    if (!valueChange and currentValue.value_ref() != rcvdValue.value_ref()) {
      *currentValue.originatorId_ref() = nodeId_;
      (*currentValue.version_ref())++;
      currentValue.ttlVersion_ref() = 0;
      valueChange = true;
    }

    // copy ttlVersion from ttl backoff map
    if (sk != keyTtlBackoffs.end()) {
      currentValue.ttlVersion_ref() = *sk->second.first.ttlVersion_ref();
    }

    // update local ttlVersion if received higher ttlVersion.
    // advertiseTtlUpdates will bump ttlVersion before advertising, so just
    // update to latest ttlVersion works fine
    if (*currentValue.ttlVersion_ref() < *rcvdValue.ttlVersion_ref()) {
      currentValue.ttlVersion_ref() = *rcvdValue.ttlVersion_ref();
      if (sk != keyTtlBackoffs.end()) {
        sk->second.first.ttlVersion_ref() = *rcvdValue.ttlVersion_ref();
      }
    }

    if (valueChange && cb != callbacks.end()) {
      (cb->second)(key, currentValue);
    }

    if (valueChange) {
      keysToAdvertise.insert(key);
    }
  } // for

  advertisePendingKeys();

  if (publication.expiredKeys_ref()->size()) {
    processExpiredKeys(publication);
  }
}

void
KvStoreClientInternal::advertisePendingKeys() {
  std::chrono::milliseconds timeout = Constants::kMaxBackoff;

  // advertise pending key for each area
  for (auto& keysToAdvertiseEntry : keysToAdvertise_) {
    auto& keysToAdvertise = keysToAdvertiseEntry.second;
    auto& area = keysToAdvertiseEntry.first;
    // Return immediately if there is nothing to advertise
    if (keysToAdvertise.empty()) {
      continue;
    }
    auto& persistedKeyVals = persistedKeyVals_[keysToAdvertiseEntry.first];

    // Build set of keys to advertise
    std::unordered_map<std::string, thrift::Value> keyVals;
    std::vector<std::string> keys;
    for (auto const& key : keysToAdvertise) {
      const auto& thriftValue = persistedKeyVals.at(key);

      // Proceed only if backoff is active
      auto& backoff = backoffs_[area].at(key);
      auto const& eventType = backoff.canTryNow() ? "Advertising" : "Skipping";
      VLOG(1) << eventType
              << " (key, version, originatorId, ttlVersion, ttl, area) "
              << folly::sformat(
                     "({}, {}, {}, {}, {})",
                     key,
                     *thriftValue.version_ref(),
                     *thriftValue.originatorId_ref(),
                     *thriftValue.ttlVersion_ref(),
                     *thriftValue.ttl_ref(),
                     area.t);
      VLOG(2) << "With value: "
              << folly::humanify(thriftValue.value_ref().value());

      if (not backoff.canTryNow()) {
        timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
        continue;
      }

      // Apply backoff
      backoff.reportError();
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

      // Set in keyVals which is going to be advertise to the kvStore.
      DCHECK(thriftValue.value_ref());
      keyVals.emplace(key, thriftValue);
      keys.push_back(key);
    }

    // Advertise to KvStore
    const auto ret = setKeysHelper(area, std::move(keyVals));
    if (ret.has_value()) {
      for (auto const& key : keys) {
        keysToAdvertise.erase(key);
      }
    } else {
      LOG(ERROR) << "Error sending SET_KEY request to KvStore.";
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling timer after " << timeout.count() << "ms.";
  advertiseKeyValsTimer_->scheduleTimeout(timeout);
}

void
KvStoreClientInternal::advertiseTtlUpdates() {
  // Build set of keys to advertise ttl updates
  auto timeout = Constants::kMaxTtlUpdateInterval;

  // advertise TTL updates for each area
  for (auto& keyTtlBackoffsEntry : keyTtlBackoffs_) {
    auto& keyTtlBackoffs = keyTtlBackoffsEntry.second;
    auto& persistedKeyVals = persistedKeyVals_[keyTtlBackoffsEntry.first];
    auto& area = keyTtlBackoffsEntry.first;

    std::unordered_map<std::string, thrift::Value> keyVals;

    for (auto& kv : keyTtlBackoffs) {
      const auto& key = kv.first;
      auto& backoff = kv.second.second;
      if (not backoff.canTryNow()) {
        VLOG(2) << "Skipping key: " << key << ", area: " << area.t;
        timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
        continue;
      }

      // Apply backoff
      backoff.reportError();
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

      auto& thriftValue = kv.second.first;
      const auto it = persistedKeyVals.find(key);
      if (it != persistedKeyVals.end()) {
        // we may have got a newer vesion for persisted key
        if (*thriftValue.version_ref() < *it->second.version_ref()) {
          thriftValue.version_ref() = *it->second.version_ref();
          thriftValue.ttlVersion_ref() = *it->second.ttlVersion_ref();
        }
      }
      // bump ttl version
      (*thriftValue.ttlVersion_ref())++;
      // Set in keyVals which is going to be advertised to the kvStore.
      DCHECK(not thriftValue.value_ref());

      VLOG(1)
          << "Advertising ttl update (key, version, originatorId, ttlVersion, area)"
          << folly::sformat(
                 " ({}, {}, {}, {}, {})",
                 key,
                 *thriftValue.version_ref(),
                 *thriftValue.originatorId_ref(),
                 *thriftValue.ttlVersion_ref(),
                 area.t);
      keyVals.emplace(key, thriftValue);
    }

    // Advertise to KvStore
    if (not keyVals.empty()) {
      const auto ret = setKeysHelper(area, std::move(keyVals));
      if (!ret.has_value()) {
        LOG(ERROR) << "Error sending SET_KEY request to KvStore.";
      }
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling ttl timer after " << timeout.count() << "ms.";
  ttlTimer_->scheduleTimeout(timeout);
}

std::optional<folly::Unit>
KvStoreClientInternal::setKeysHelper(
    AreaId const& area,
    std::unordered_map<std::string, thrift::Value> keyVals) {
  CHECK(kvStore_);

  // Return if nothing to advertise.
  if (keyVals.empty()) {
    return folly::Unit();
  }

  // Debugging purpose print-out
  for (auto const& kv : keyVals) {
    VLOG(3) << "Advertising key: " << kv.first
            << ", version: " << *kv.second.version_ref()
            << ", originatorId: " << *kv.second.originatorId_ref()
            << ", ttlVersion: " << *kv.second.ttlVersion_ref() << ", val: "
            << (kv.second.value_ref().has_value() ? "valid" : "null")
            << ", area: " << area.t;
    if (not kv.second.value_ref().has_value()) {
      // avoid empty optinal exception
      continue;
    }
  }

  thrift::KeySetParams params;
  *params.keyVals_ref() = std::move(keyVals);

  try {
    kvStore_->setKvStoreKeyVals(area, params).get();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to set key-val from KvStore. Exception: "
               << ex.what();
    return std::nullopt;
  }
  return folly::Unit();
}

} // namespace openr
