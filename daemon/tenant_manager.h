/*
 *     Copyright 2021-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */
#pragma once

#include <nlohmann/json_fwd.hpp>
#include <memory>

namespace cb::rbac {
struct UserIdent;
}
class Tenant;

namespace cb::sasl::pwdb::user {
struct Limits;
}

class TenantManager {
public:
    static void startup();
    static void shutdown();

    static std::shared_ptr<Tenant> get(const cb::rbac::UserIdent& ident,
                                       bool create = true);
    static void setLimits(const cb::rbac::UserIdent& ident,
                          const cb::sasl::pwdb::user::Limits& limits);

    static nlohmann::json to_json();
};
