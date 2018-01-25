////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "UserManager.h"

#include "Agency/AgencyComm.h"
#include "Aql/Query.h"
#include "Aql/QueryString.h"
#include "Auth/Handler.h"
#include "Basics/ReadLocker.h"
#include "Basics/ReadUnlocker.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/tri-strings.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "Logger/Logger.h"
#include "Random/UniformCharacter.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/InitDatabaseFeature.h"
#include "Ssl/SslInterface.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::velocypack;
using namespace arangodb::rest;

auth::UserManager::UserManager() : _outdated(true),
_queryRegistry(nullptr),
_authHandler(nullptr) {}

auth::UserManager::UserManager(std::unique_ptr<auth::Handler>&& handler)
    : _outdated(true),
      _queryRegistry(nullptr),
      _authHandler(handler.release()) {}

auth::UserManager::~UserManager() {
  delete _authHandler;
}

// Parse the users
static auth::UserMap ParseUsers(VPackSlice const& slice) {
  TRI_ASSERT(slice.isArray());
  auth::UserMap result;
  for (VPackSlice const& authSlice : VPackArrayIterator(slice)) {
    VPackSlice s = authSlice.resolveExternal();

    if (s.hasKey("source") && s.get("source").isString() &&
        s.get("source").copyString() == "LDAP") {
      LOG_TOPIC(TRACE, arangodb::Logger::CONFIG)
          << "LDAP: skip user in collection _users: "
          << s.get("user").copyString();
      continue;
    }

    // we also need to insert inactive users into the cache here
    // otherwise all following update/replace/remove operations on the
    // user will fail
    auth::User user = auth::User::fromDocument(s);
    result.emplace(user.username(), std::move(user));
  }
  return result;
}

static std::shared_ptr<VPackBuilder> QueryAllUsers(
    aql::QueryRegistry* queryRegistry) {
  TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->systemDatabase();
  if (vocbase == nullptr) {
    LOG_TOPIC(DEBUG, arangodb::Logger::FIXME) << "system database is unknown";
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  // we cannot set this execution context, otherwise the transaction
  // will ask us again for permissions and we get a deadlock
  ExecContextScope scope(ExecContext::superuser());
  std::string const queryStr("FOR user IN _users RETURN user");
  auto emptyBuilder = std::make_shared<VPackBuilder>();
  arangodb::aql::Query query(false, vocbase,
                             arangodb::aql::QueryString(queryStr), emptyBuilder,
                             emptyBuilder, arangodb::aql::PART_MAIN);

  LOG_TOPIC(DEBUG, arangodb::Logger::FIXME)
      << "starting to load authentication and authorization information";
  auto queryResult = query.execute(queryRegistry);

  if (queryResult.code != TRI_ERROR_NO_ERROR) {
    if (queryResult.code == TRI_ERROR_REQUEST_CANCELED ||
        (queryResult.code == TRI_ERROR_QUERY_KILLED)) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_REQUEST_CANCELED);
    }
    THROW_ARANGO_EXCEPTION_MESSAGE(
        queryResult.code, "Error executing user query: " + queryResult.details);
  }

  VPackSlice usersSlice = queryResult.result->slice();

  if (usersSlice.isNone()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  } else if (!usersSlice.isArray()) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME)
        << "cannot read users from _users collection";
    return std::shared_ptr<VPackBuilder>();
  }

  return queryResult.result;
}

static VPackBuilder QueryUser(aql::QueryRegistry* queryRegistry,
                              std::string const& user) {
  TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->systemDatabase();

  if (vocbase == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FAILED, "_system db is unknown");
  }

  // we cannot set this execution context, otherwise the transaction
  // will ask us again for permissions and we get a deadlock
  ExecContextScope scope(ExecContext::superuser());
  std::string const queryStr("FOR u IN _users FILTER u.user == @name RETURN u");
  auto emptyBuilder = std::make_shared<VPackBuilder>();

  VPackBuilder binds;
  binds.openObject();
  binds.add("name", VPackValue(user));
  binds.close();  // obj
  arangodb::aql::Query query(false, vocbase,
                             arangodb::aql::QueryString(queryStr),
                             std::make_shared<VPackBuilder>(binds),
                             emptyBuilder, arangodb::aql::PART_MAIN);

  auto queryResult = query.execute(queryRegistry);

  if (queryResult.code != TRI_ERROR_NO_ERROR) {
    if (queryResult.code == TRI_ERROR_REQUEST_CANCELED ||
        (queryResult.code == TRI_ERROR_QUERY_KILLED)) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_REQUEST_CANCELED);
    }
    THROW_ARANGO_EXCEPTION_MESSAGE(
        queryResult.code, "Error executing user query: " + queryResult.details);
  }

  VPackSlice usersSlice = queryResult.result->slice();

  if (usersSlice.isNone() || !usersSlice.isArray()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (usersSlice.length() == 0) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_USER_NOT_FOUND);
  }

  VPackSlice doc = usersSlice.at(0);

  if (doc.isExternal()) {
    doc = doc.resolveExternals();
  }
  VPackBuilder result;
  result.add(doc);
  return result;
}

static void ConvertLegacyFormat(VPackSlice doc, VPackBuilder& result) {
  if (doc.isExternal()) {
    doc = doc.resolveExternals();
  }
  VPackSlice authDataSlice = doc.get("authData");
  VPackObjectBuilder b(&result, true);
  result.add("user", doc.get("user"));
  result.add("active", authDataSlice.get("active"));
  VPackSlice extra = doc.get("userData");
  result.add("extra", extra.isNone() ? VPackSlice::emptyObjectSlice() : extra);
}

// private, will acquire _userCacheLock in write-mode and release it.
// will also aquire _loadFromDBLock and release it
void auth::UserManager::loadFromDB() {
  TRI_ASSERT(_queryRegistry != nullptr);
  TRI_ASSERT(ServerState::instance()->isSingleServerOrCoordinator());
  if (!ServerState::instance()->isSingleServerOrCoordinator()) {
    _outdated = false;  // should not get here
    return;
  }

  if (!_outdated) {
    return;
  }
  MUTEX_LOCKER(guard, _loadFromDBLock);  // must be first
  if (!_outdated) {                       // double check after we got the lock
    return;
  }

  try {
    std::shared_ptr<VPackBuilder> builder = QueryAllUsers(_queryRegistry);
    if (builder) {
      VPackSlice usersSlice = builder->slice();
      if (usersSlice.length() != 0) {
        UserMap usermap = ParseUsers(usersSlice);

        {  // cannot invalidate token cache while holding _userCache write lock
          WRITE_LOCKER(writeGuard, _userCacheLock);  // must be second
          // never delete non-local users
          for (auto pair = _userCache.cbegin(); pair != _userCache.cend();) {
            if (pair->second.source() == auth::Source::LOCAL) {
              pair = _userCache.erase(pair);
            } else {
              pair++;
            }
          }
          _userCache.insert(usermap.begin(), usermap.end());
        }

        _outdated = false;
        AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
      }
    }
  } catch (std::exception const& ex) {
    LOG_TOPIC(WARN, Logger::AUTHENTICATION)
        << "Exception when loading users from db: " << ex.what();
    _outdated = true;
    AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
  } catch (...) {
    LOG_TOPIC(TRACE, Logger::AUTHENTICATION)
        << "Exception when loading users from db";
    _outdated = true;
    AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
  }
}

// private, must be called with _userCacheLock in write mode
// this method can only be called by users with access to the _system collection
Result auth::UserManager::storeUserInternal(auth::User&& entry, bool replace) {
  if (entry.source() != auth::Source::LOCAL) {
    return TRI_ERROR_USER_EXTERNAL;
  }

  VPackBuilder data = entry.toVPackBuilder();
  bool hasKey = data.slice().hasKey(StaticStrings::KeyString);
  TRI_ASSERT((replace && hasKey) || (!replace && !hasKey));

  TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->systemDatabase();
  if (vocbase == nullptr) {
    return Result(TRI_ERROR_INTERNAL);
  }

  // we cannot set this execution context, otherwise the transaction
  // will ask us again for permissions and we get a deadlock
  ExecContextScope scope(ExecContext::superuser());
  auto ctx = transaction::StandaloneContext::Create(vocbase);
  SingleCollectionTransaction trx(ctx, TRI_COL_NAME_USERS,
                                  AccessMode::Type::WRITE);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();
  if (res.ok()) {
    OperationOptions opts;
    opts.returnNew = true;
    opts.ignoreRevs = false;
    opts.mergeObjects = false;
    OperationResult opres =
        replace ? trx.replace(TRI_COL_NAME_USERS, data.slice(), opts)
                : trx.insert(TRI_COL_NAME_USERS, data.slice(), opts);
    res = trx.finish(opres.result);
    if (res.ok()) {
      VPackSlice userDoc = opres.slice();
      TRI_ASSERT(userDoc.isObject() && userDoc.hasKey("new"));
      userDoc = userDoc.get("new");
      if (userDoc.isExternal()) {
        userDoc = userDoc.resolveExternal();
      }

      // parse user including document _key
      auth::User created = auth::User::fromDocument(userDoc);
      TRI_ASSERT(!created.key().empty() && created.rev() != 0);
      TRI_ASSERT(created.username() == entry.username());
      TRI_ASSERT(created.isActive() == entry.isActive());
      TRI_ASSERT(created.passwordHash() == entry.passwordHash());
      TRI_ASSERT(!replace || created.key() == entry.key());

      if (!_userCache.emplace(entry.username(), std::move(created)).second) {
        // insertion should always succeed, but...
        _userCache.erase(entry.username());
        _userCache.emplace(entry.username(), auth::User::fromDocument(userDoc));
      }
    } else if (res.is(TRI_ERROR_ARANGO_CONFLICT)) {  // user was outdated
      _userCache.erase(entry.username());
      _outdated = true;
    }
  }
  return res;
}

// -----------------------------------------------------------------------------
// -- SECTION --                                                          public
// -----------------------------------------------------------------------------

// only call from the boostrap feature, must be sure to be the only one
void auth::UserManager::createRootUser() {
  loadFromDB();

  MUTEX_LOCKER(guard, _loadFromDBLock);      // must be first
  WRITE_LOCKER(writeGuard, _userCacheLock);  // must be second
  auto it = _userCache.find("root");
  if (it != _userCache.end()) {
    LOG_TOPIC(TRACE, Logger::AUTHENTICATION) << "Root already exists";
    return;
  }
  TRI_ASSERT(_userCache.empty());

  try {
    // Attention:
    // the root user needs to have a specific rights grant
    // to the "_system" database, otherwise things break
    auto initDatabaseFeature =
        application_features::ApplicationServer::getFeature<
            InitDatabaseFeature>("InitDatabase");

    TRI_ASSERT(initDatabaseFeature != nullptr);

    auth::User user = auth::User::newUser(
        "root", initDatabaseFeature->defaultPassword(), auth::Source::LOCAL);
    user.setActive(true);
    user.grantDatabase(StaticStrings::SystemDatabase, auth::Level::RW);
    user.grantDatabase("*", auth::Level::RW);
    user.grantCollection("*", "*", auth::Level::RW);
    storeUserInternal(std::move(user), false);
  } catch (...) {
    // No action
  }
}

VPackBuilder auth::UserManager::allUsers() {
  // will query db directly, no need for _userCacheLock
  std::shared_ptr<VPackBuilder> users;
  {
    TRI_ASSERT(_queryRegistry != nullptr);
    users = QueryAllUsers(_queryRegistry);
  }

  VPackBuilder result;
  VPackArrayBuilder a(&result);
  if (users && !users->isEmpty()) {
    for (VPackSlice const& doc : VPackArrayIterator(users->slice())) {
      ConvertLegacyFormat(doc, result);
    }
  }
  return result;
}

/// Trigger eventual reload, user facing API call
void auth::UserManager::reloadAllUsers() {
  if (!ServerState::instance()->isCoordinator()) {
    // will reload users on next suitable query
    return;
  }

  // tell other coordinators to reload as well
  AgencyComm agency;

  AgencyWriteTransaction incrementVersion({AgencyOperation(
      "Sync/UserVersion", AgencySimpleOperationType::INCREMENT_OP)});

  int maxTries = 10;

  while (maxTries-- > 0) {
    AgencyCommResult result =
        agency.sendTransactionWithFailover(incrementVersion);
    if (result.successful()) {
      return;
    }
  }

  LOG_TOPIC(WARN, Logger::AUTHENTICATION)
      << "Sync/UserVersion could not be updated";
}

Result auth::UserManager::storeUser(bool replace, std::string const& username,
                                    std::string const& pass, bool active,
                                    VPackSlice extras) {
  if (username.empty()) {
    return TRI_ERROR_USER_INVALID_NAME;
  }

  loadFromDB();
  WRITE_LOCKER(writeGuard, _userCacheLock);
  auto const& it = _userCache.find(username);

  if (replace && it == _userCache.end()) {
    return TRI_ERROR_USER_NOT_FOUND;
  } else if (!replace && it != _userCache.end()) {
    return TRI_ERROR_USER_DUPLICATE;
  }

  std::string oldKey;  // will only be populated during replace
  if (replace) {
    auth::User const& oldEntry = it->second;
    oldKey = oldEntry.key();
    if (oldEntry.source() == auth::Source::LDAP) {
      return TRI_ERROR_USER_EXTERNAL;
    }
  }

  auth::User user = auth::User::newUser(username, pass, auth::Source::LOCAL);
  user.setActive(active);
  if (extras.isObject() && !extras.isEmptyObject()) {
    user.setUserData(VPackBuilder(extras));
  }

  if (replace) {
    TRI_ASSERT(!oldKey.empty());
    user._key = std::move(oldKey);
  }

  Result r = storeUserInternal(std::move(user), replace);
  if (r.ok()) {
    reloadAllUsers();
  }
  return r;
}

Result auth::UserManager::enumerateUsers(
    std::function<bool(auth::User&)>&& func) {
  loadFromDB();

  std::vector<auth::User> toUpdate;
  {  // users are later updated with rev ID for consistency
    READ_LOCKER(readGuard, _userCacheLock);
    for (auto& it : _userCache) {
      if (it.second.source() == auth::Source::LDAP) {
        continue;
      }
      auth::User user(it.second);
      TRI_ASSERT(!user.key().empty() && user.rev() != 0);
      if (func(user)) {
        toUpdate.emplace_back(std::move(user));
      }
    }
  }
  Result res;
  {
    WRITE_LOCKER(writeGuard, _userCacheLock);
    for (auth::User& u : toUpdate) {
      res = storeUserInternal(std::move(u), true);
      if (res.fail()) {
        break;  // do not return, still need to invalidate token cache
      }
    }
  }

  // cannot invalidate token cache while holding _userCache write lock
  if (!toUpdate.empty()) {
    AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
    reloadAllUsers();  // trigger auth reload in cluster
  }
  return res;
}

Result auth::UserManager::updateUser(std::string const& username,
                                     UserCallback&& func) {
  if (username.empty()) {
    return TRI_ERROR_USER_NOT_FOUND;
  }

  loadFromDB();

  // we require a consistent view on the user object
  WRITE_LOCKER(writeGuard, _userCacheLock);
  
  auto it = _userCache.find(username);
  if (it == _userCache.end()) {
    return TRI_ERROR_USER_NOT_FOUND;
  } else if (it->second.source() == auth::Source::LDAP) {
    return TRI_ERROR_USER_EXTERNAL;
  }

  auth::User user = it->second;
  TRI_ASSERT(!user.key().empty() && user.key() != 0);
  Result r = func(user);
  if (r.fail()) {
    return r;
  }
  r = storeUserInternal(std::move(user), /*replace*/ true);
  // cannot invalidate token cache while holding _userCache write lock
  writeGuard.unlock();

  if (r.ok() || r.is(TRI_ERROR_ARANGO_CONFLICT)) {
    // must also clear the basic cache here because the secret may be
    // invalid now if the password was changed
    AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
    if (r.ok()) {
      reloadAllUsers();  // trigger auth reload in cluster
    }
  }
  return r;
}

Result auth::UserManager::accessUser(std::string const& user,
                                     ConstUserCallback&& func) {
  if (user.empty()) {
    return TRI_ERROR_USER_NOT_FOUND;
  }

  loadFromDB();
  READ_LOCKER(readGuard, _userCacheLock);
  auto it = _userCache.find(user);
  if (it != _userCache.end()) {
    return func(it->second);
  }
  return TRI_ERROR_USER_NOT_FOUND;
}

VPackBuilder auth::UserManager::serializeUser(std::string const& user) {
  loadFromDB();
  // will query db directly, no need for _userCacheLock
  VPackBuilder doc = QueryUser(_queryRegistry, user);
  VPackBuilder result;
  if (!doc.isEmpty()) {
    ConvertLegacyFormat(doc.slice(), result);
  }
  return result;
}

static Result RemoveUserInternal(auth::User const& entry) {
  TRI_ASSERT(!entry.key().empty());
  TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->systemDatabase();

  if (vocbase == nullptr) {
    return Result(TRI_ERROR_INTERNAL);
  }

  // we cannot set this execution context, otherwise the transaction
  // will ask us again for permissions and we get a deadlock
  ExecContextScope scope(ExecContext::superuser());
  auto ctx = transaction::StandaloneContext::Create(vocbase);
  SingleCollectionTransaction trx(ctx, TRI_COL_NAME_USERS,
                                  AccessMode::Type::WRITE);

  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();

  if (res.ok()) {
    VPackBuilder builder;
    {
      VPackObjectBuilder guard(&builder);
      builder.add(StaticStrings::KeyString, VPackValue(entry.key()));
      // TODO maybe protect with a revision ID?
    }

    OperationResult result =
        trx.remove(TRI_COL_NAME_USERS, builder.slice(), OperationOptions());
    res = trx.finish(result.result);
  }

  return res;
}

Result auth::UserManager::removeUser(std::string const& user) {
  if (user.empty()) {
    return TRI_ERROR_USER_NOT_FOUND;
  }

  if (user == "root") {
    return TRI_ERROR_FORBIDDEN;
  }

  loadFromDB();

  WRITE_LOCKER(writeGuard, _userCacheLock);
  auto const& it = _userCache.find(user);
  if (it == _userCache.end()) {
    return TRI_ERROR_USER_NOT_FOUND;
  }

  auth::User const& oldEntry = it->second;
  if (oldEntry.source() == auth::Source::LDAP) {
    return TRI_ERROR_USER_EXTERNAL;
  }

  Result res = RemoveUserInternal(oldEntry);
  if (res.ok()) {
    _userCache.erase(it);
  }

  // cannot invalidate token cache while holding _userCache write lock
  AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
  reloadAllUsers();  // trigger auth reload in cluster

  return res;
}

Result auth::UserManager::removeAllUsers() {
  loadFromDB();

  Result res;
  {
    // do not get into race conditions with loadFromDB
    MUTEX_LOCKER(guard, _loadFromDBLock);  // must be first
    WRITE_LOCKER(writeGuard, _userCacheLock);    // must be second

    for (auto pair = _userCache.cbegin(); pair != _userCache.cend();) {
      auto const& oldEntry = pair->second;
      if (oldEntry.source() == auth::Source::LOCAL) {
        res = RemoveUserInternal(oldEntry);
        if (!res.ok()) {
          break;  // don't return still need to invalidate token cache
        }
        pair = _userCache.erase(pair);
      } else {
        pair++;
      }
    }
    _outdated = true;
  }

  // cannot invalidate token cache while holding _userCache write lock
  AuthenticationFeature::instance()->tokenCache()->invalidateBasicCache();
  reloadAllUsers();
  return res;
}

bool auth::UserManager::checkPassword(std::string const& username,
                                      std::string const& password) {
  // AuthResult result(username);
  if (username.empty() || StringUtils::isPrefix(username, ":role:")) {
    return false;
  }

  loadFromDB();

  READ_LOCKER(readGuard, _userCacheLock);
  auto it = _userCache.find(username);

  // using local users might be forbidden
  AuthenticationFeature* af = AuthenticationFeature::instance();
  if (it != _userCache.end() && (it->second.source() == auth::Source::LOCAL) &&
      af != nullptr && !af->localAuthentication()) {
    return false;
  }
  
  if (it != _userCache.end() && it->second.source() == auth::Source::LOCAL) {
    auth::User const& auth = it->second;
    if (auth.isActive()) {
      return auth.checkPassword(password);
    }
  }
  if (it == _userCache.end() && _authHandler == nullptr) {
    return false; // nothing more to do here
  }
  
  // handle LDAP based authentication
  if (it == _userCache.end() || (it->second.source() != auth::Source::LOCAL)) {
    
    TRI_ASSERT(_authHandler != nullptr);
    auth::HandlerResult authResult =
    _authHandler->authenticate(username, password);
    
    if (!authResult.ok()) {
      if (it != _userCache.end()) {  // erase invalid user
        // upgrade read-lock to a write-lock
        readGuard.unlock();
        WRITE_LOCKER(writeGuard, _userCacheLock);
        _userCache.erase(username);  // `it` may be invalid already
      }
      return false;
    }
      
    // user authed, add to _userCache
    if (authResult.source() == auth::Source::LDAP) {
      auth::User user =
      auth::User::newUser(username, password, auth::Source::LDAP);
      user.setRoles(authResult.roles());
      for (auto const& al : authResult.permissions()) {
        user.grantDatabase(al.first, al.second);
      }
      
      // upgrade read-lock to a write-lock
      readGuard.unlock();
      WRITE_LOCKER(writeGuard, _userCacheLock);
      
      it = _userCache.find(username);  // `it` may be invalid already
      if (it != _userCache.end()) {
        it->second = std::move(user);
      } else {
        auto r = _userCache.emplace(username, std::move(user));
        if (!r.second) {
          return false;  // insertion failed
        }
        it = r.first;
      }
      return it->second.isActive();
      /*auth::User const& auth = it->second;
      if (auth.isActive()) {
        return auth.checkPassword(password);
      }
      return false;  // inactive user*/
    }
  }

  return false;
}

// worker function for configuredDatabaseAuthLevel
// must only be called with the read-lock on _userCacheLock being held
auth::Level auth::UserManager::configuredDatabaseAuthLevelInternal(
    std::string const& username, std::string const& dbname,
    size_t depth) const {
  auto it = _userCache.find(username);

  if (it == _userCache.end()) {
    return auth::Level::NONE;
  }

  auto const& entry = it->second;
  auth::Level level = entry.databaseAuthLevel(dbname);

#ifdef USE_ENTERPRISE
  // check all roles and use the highest permission from them
  for (auto const& role : entry.roles()) {
    if (level == auth::Level::RW) {
      // we already have highest permission
      break;
    }

    // recurse into function, but only one level deep.
    // this allows us to avoid endless recursion without major overhead
    if (depth == 0) {
      auth::Level roleLevel =
          configuredDatabaseAuthLevelInternal(role, dbname, depth + 1);

      if (level == auth::Level::NONE) {
        // use the permission of the role we just found
        level = roleLevel;
      }
    }
  }
#endif
  return level;
}

auth::Level auth::UserManager::configuredDatabaseAuthLevel(
    std::string const& username, std::string const& dbname) {
  loadFromDB();
  READ_LOCKER(readGuard, _userCacheLock);
  return configuredDatabaseAuthLevelInternal(username, dbname, 0);
}

auth::Level auth::UserManager::canUseDatabase(std::string const& username,
                                              std::string const& dbname) {
  auth::Level level = configuredDatabaseAuthLevel(username, dbname);
  static_assert(auth::Level::RO < auth::Level::RW, "ro < rw");
  if (level > auth::Level::RO && !ServerState::writeOpsEnabled()) {
    return auth::Level::RO;
  }
  return level;
}

auth::Level auth::UserManager::canUseDatabaseNoLock(std::string const& username,
                                                    std::string const& dbname) {
  auth::Level level = configuredDatabaseAuthLevelInternal(username, dbname, 0);
  static_assert(auth::Level::RO < auth::Level::RW, "ro < rw");
  if (level > auth::Level::RO && !ServerState::writeOpsEnabled()) {
    return auth::Level::RO;
  }
  return level;
}

// internal method called by configuredCollectionAuthLevel
// asserts that collection name is non-empty and already translated
// from collection id to name
auth::Level auth::UserManager::configuredCollectionAuthLevelInternal(
    std::string const& username, std::string const& dbname,
    std::string const& coll, size_t depth) const {
  // we must have got a non-empty collection name when we get here
  TRI_ASSERT(coll[0] < '0' || coll[0] > '9');

  auto it = _userCache.find(username);
  if (it == _userCache.end()) {
    return auth::Level::NONE;
  }

  auto const& entry = it->second;
  auth::Level level = entry.collectionAuthLevel(dbname, coll);

#ifdef USE_ENTERPRISE
  for (auto const& role : entry.roles()) {
    if (level == auth::Level::RW) {
      // we already have highest permission
      return level;
    }

    // recurse into function, but only one level deep.
    // this allows us to avoid endless recursion without major overhead
    if (depth == 0) {
      auth::Level roleLevel =
          configuredCollectionAuthLevelInternal(role, dbname, coll, depth + 1);

      if (level == auth::Level::NONE) {
        // use the permission of the role we just found
        level = roleLevel;
      }
    }
  }
#endif
  return level;
}

auth::Level auth::UserManager::configuredCollectionAuthLevel(
    std::string const& username, std::string const& dbname, std::string coll) {
  if (coll.empty()) {
    // no collection name given
    return auth::Level::NONE;
  }
  if (coll[0] >= '0' && coll[0] <= '9') {
    coll = DatabaseFeature::DATABASE->translateCollectionName(dbname, coll);
  }

  loadFromDB();
  READ_LOCKER(readGuard, _userCacheLock);

  return configuredCollectionAuthLevelInternal(username, dbname, coll, 0);
}

auth::Level auth::UserManager::canUseCollection(std::string const& username,
                                                std::string const& dbname,
                                                std::string const& coll) {
  if (coll.empty()) {
    // no collection name given
    return auth::Level::NONE;
  }

  auth::Level level = configuredCollectionAuthLevel(username, dbname, coll);
  static_assert(auth::Level::RO < auth::Level::RW, "ro < rw");
  if (level > auth::Level::RO && !ServerState::writeOpsEnabled()) {
    return auth::Level::RO;
  }
  return level;
}

auth::Level auth::UserManager::canUseCollectionNoLock(
    std::string const& username, std::string const& dbname,
    std::string const& coll) {
  if (coll.empty()) {
    // no collection name given
    return auth::Level::NONE;
  }

  auth::Level level;
  if (coll[0] >= '0' && coll[0] <= '9') {
    std::string tmpColl =
        DatabaseFeature::DATABASE->translateCollectionName(dbname, coll);
    level = configuredCollectionAuthLevelInternal(username, dbname, tmpColl, 0);
  } else {
    level = configuredCollectionAuthLevelInternal(username, dbname, coll, 0);
  }

  static_assert(auth::Level::RO < auth::Level::RW, "ro < rw");
  if (level > auth::Level::RO && !ServerState::writeOpsEnabled()) {
    return auth::Level::RO;
  }
  return level;
}

/// Only used for testing
void auth::UserManager::setAuthInfo(auth::UserMap const& newMap) {
  MUTEX_LOCKER(guard, _loadFromDBLock);       // must be first
  WRITE_LOCKER(writeGuard, _userCacheLock);  // must be second
  _userCache = newMap;
  _outdated = false;
}