////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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

#include "UpgradeFeature.h"

#include "Cluster/ClusterFeature.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/DatabaseFeature.h"
#include "V8/v8-globals.h"
#include "V8Server/V8Context.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-vocbase.h"
#include "VocBase/server.h"
#include "Wal/LogfileManager.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

UpgradeFeature::UpgradeFeature(
    ApplicationServer* server, int* result,
    std::vector<std::string> const& nonServerFeatures)
    : ApplicationFeature(server, "Upgrade"),
      _upgrade(false),
      _upgradeCheck(true),
      _result(result),
      _nonServerFeatures(nonServerFeatures) {
  setOptional(false);
  requiresElevatedPrivileges(false);
  startsAfter("CheckVersion");
  startsAfter("Cluster");
  startsAfter("Database");
  startsAfter("V8Dealer");
}

void UpgradeFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::collectOptions";

  options->addSection("database", "Configure the database");

  options->addOption("--database.upgrade",
                     "perform a database upgrade if necessary",
                     new BooleanParameter(&_upgrade, true));

  options->addHiddenOption("--database.upgrade-check",
                           "skip a database upgrade",
                           new BooleanParameter(&_upgradeCheck, true));
}

void UpgradeFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::validateOptions";

  if (_upgrade && !_upgradeCheck) {
    LOG(FATAL) << "cannot specify both '--database.upgrade true' and "
                  "'--database.upgrade-check false'";
    FATAL_ERROR_EXIT();
  }

  if (!_upgrade) {
    LOG(TRACE) << "executing upgrade check: not disabling server features";
    return;
  }

  LOG(TRACE) << "executing upgrade procedure: disabling server features";

  ApplicationServer::disableFeatures(_nonServerFeatures);

  DatabaseFeature* database = dynamic_cast<DatabaseFeature*>(
      ApplicationServer::lookupFeature("Database"));
  database->disableReplicationApplier();
  database->enableUpgrade();

  ClusterFeature* cluster = dynamic_cast<ClusterFeature*>(
      ApplicationServer::lookupFeature("Cluster"));
  cluster->disable();
}

void UpgradeFeature::start() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::start";

  // open the log file for writing
  if (!wal::LogfileManager::instance()->open()) {
    LOG(FATAL) << "Unable to finish WAL recovery procedure";
    FATAL_ERROR_EXIT();
  }

  // upgrade the database
  if (_upgradeCheck) {
    upgradeDatabase();
  }

  // and force shutdown
  if (_upgrade) {
    server()->beginShutdown();
  }
}

void UpgradeFeature::upgradeDatabase() {
  LOG(TRACE) << "starting database init/upgrade";

  auto* server = DatabaseFeature::DATABASE->server();
  auto* systemVocbase = DatabaseFeature::DATABASE->vocbase();

  // enter context and isolate
  {
    V8Context* context =
        V8DealerFeature::DEALER->enterContext(systemVocbase, true, 0);
    v8::HandleScope scope(context->_isolate);
    auto localContext =
        v8::Local<v8::Context>::New(context->_isolate, context->_context);
    localContext->Enter();

    {
      v8::Context::Scope contextScope(localContext);

      // run upgrade script
      LOG(DEBUG) << "running database init/upgrade";

      auto unuser(server->_databasesProtector.use());
      auto theLists = server->_databasesLists.load();

      for (auto& p : theLists->_databases) {
        TRI_vocbase_t* vocbase = p.second;

        // special check script to be run just once in first thread (not in
        // all) but for all databases
        v8::HandleScope scope(context->_isolate);

        v8::Handle<v8::Object> args = v8::Object::New(context->_isolate);
        args->Set(TRI_V8_ASCII_STRING2(context->_isolate, "upgrade"),
                  v8::Boolean::New(context->_isolate, _upgrade));

        localContext->Global()->Set(
            TRI_V8_ASCII_STRING2(context->_isolate, "UPGRADE_ARGS"), args);

        bool ok = TRI_UpgradeDatabase(vocbase, localContext);

        if (!ok) {
          if (localContext->Global()->Has(
                  TRI_V8_ASCII_STRING2(context->_isolate, "UPGRADE_STARTED"))) {
            localContext->Exit();
            if (_upgrade) {
              LOG(FATAL) << "Database '" << vocbase->_name
                         << "' upgrade failed. Please inspect the logs from "
                            "the upgrade procedure";
              FATAL_ERROR_EXIT();
            } else {
              LOG(FATAL) << "Database '" << vocbase->_name
                         << "' needs upgrade. Please start the server with the "
                            "--upgrade option";
              FATAL_ERROR_EXIT();
            }
          } else {
            LOG(FATAL) << "JavaScript error during server start";
            FATAL_ERROR_EXIT();
          }

          LOG(DEBUG) << "database '" << vocbase->_name << "' init/upgrade done";
        }
      }
    }

    // finally leave the context. otherwise v8 will crash with assertion failure
    // when we delete
    // the context locker below
    localContext->Exit();
    V8DealerFeature::DEALER->exitContext(context);
  }

  if (_upgrade) {
    *_result = EXIT_SUCCESS;
    LOG(INFO) << "database upgrade passed";
  }

  // and return from the context
  LOG(TRACE) << "finished database init/upgrade";
}