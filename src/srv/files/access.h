#pragma once

#include <fileservice/protocol.h>

namespace fileservice {

// The two masks govern different nodes: operations here and grants below here.
struct Access {
    uint32_t rights;
    uint32_t ceiling;

    bool allows(uint32_t operation) const { return (rights & operation) == operation; }

    uint32_t grant(Access child) const {
        const auto requested = child.rights | child.ceiling;
        if (requested & ~ALL_RIGHTS) { return UNSUPPORTED; }
        const auto bytes = child.rights & READ_WRITE;
        if (bytes != 0 && bytes != READ_BYTES && bytes != APPEND_BYTES && bytes != READ_WRITE) { return UNSUPPORTED; }
        return (requested & ~ceiling) ? ACCESS_DENIED : 0;
    }
};

}  // namespace fileservice
