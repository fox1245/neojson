// Lazy/COW semantics, materialize() to unpin large backing yyjson documents, SharedString pinning, and array_t.

#include "neojson/json.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using json = neojson::json;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

void run() {
    constexpr std::size_t kPayloadBytes = 256u * 1024u;
    const std::string large_payload(kPayloadBytes, 'x');
    const std::string envelope_text =
        std::string(R"({"meta":{"id":42,"tags":["fast","cow"]},"payload":")") + large_payload + R"("})";

    // Parse a 256 KiB lazy document backed by an immutable yyjson buffer.
    json envelope = json::parse(envelope_text);
    json cow_copy = envelope;

    // Prefer std::as_const for read-only inspection to avoid detaching shared storage.
    const json& ro_copy = std::as_const(cow_copy);
    require(ro_copy.at("meta").at("id").get<int>() == 42, "const read sees shared id");
    const auto* payload_ptr = ro_copy.at("payload").get_ptr<const json::string_t*>();
    require(payload_ptr != nullptr && payload_ptr->size() == kPayloadBytes, "const string pointer length matches");

    // Copying out via std::as_const before mutating avoids invalidating references if shared storage detaches.
    json shared_obj = envelope;
    json cloned_meta = std::as_const(shared_obj)["meta"];
    shared_obj["meta_backup"] = std::move(cloned_meta);
    require(shared_obj.at("meta_backup").at("id") == 42, "copy-then-assign preserves value");
    require(!std::as_const(envelope).contains("meta_backup"), "COW keeps original envelope isolated");

    // Retaining an unmaterialized slice pins the large yyjson document; call materialize() before keeping long-term.
    json retained_meta = std::as_const(envelope).at("meta");
    retained_meta.materialize();
    envelope = json();
    cow_copy = json();
    shared_obj = json();

    // Exact container compatibility: neojson::json::array_t uses pool_allocator (not std::vector<json>).
    const json::array_t* tags = std::as_const(retained_meta).at("tags").get_ptr<const json::array_t*>();
    require(tags != nullptr && tags->size() == 2 && tags->at(0) == "fast" && tags->at(1) == "cow",
            "materialized subtree survives dropping backing documents");

    // Non-const get_ptr<string_t*>() detaches if shared and pins the SharedString unshareable.
    json note = "initial";
    json prior_copy = note;
    json::string_t* mutable_note = note.get_ptr<json::string_t*>();
    require(mutable_note != nullptr, "mutable string pointer obtained");
    *mutable_note += "-pinned";
    json post_pin_copy = note;
    *mutable_note += "-edited";
    require(prior_copy == "initial", "prior copy unaffected by mutable handle");
    require(post_pin_copy == "initial-pinned", "post-pin copy clones eagerly instead of aliasing");
    require(note == "initial-pinned-edited", "pinned string reflects direct mutations");

    std::cout << "lazy_cow_materialize: ok\n";
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& err) {
        std::cerr << "lazy_cow_materialize failed: " << err.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
