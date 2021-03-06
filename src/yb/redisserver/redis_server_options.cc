// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/redisserver/redis_server_options.h"

#include "yb/redisserver/redis_rpc.h"
#include "yb/redisserver/redis_server.h"

namespace yb {
namespace redisserver {

RedisServerOptions::RedisServerOptions() {
  server_type = "tserver";
  rpc_opts.default_port = RedisServer::kDefaultPort;
  connection_context_factory = &std::make_unique<RedisConnectionContext>;
}

} // namespace redisserver
} // namespace yb
