// M2-04 fusion link-closure probe (DEC-013): links the perception session and
// its transitive image/cache dependencies; the readelf NEEDED check asserts
// the whole fusion closure still loads only standard-library runtime.
#include <mirador/perception_session.hpp>

int main() {
    mirador::PerceptionSessionOptions options;
    options.source_id = "closure-probe";
    mirador::PerceptionSession session = mirador::PerceptionSession::create(options).take_value();
    return static_cast<int>(session.result_cache().entry_count());
}
