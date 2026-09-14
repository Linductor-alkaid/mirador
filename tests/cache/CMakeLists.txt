# Unit tests for mirador::cache public contracts (M1-07, M2-02).

mirador_add_test(
    NAME mirador.cache.frame_cache
    SOURCES frame_cache_test.cpp
    LIBS mirador::cache
    LABELS unit)

mirador_add_test(
    NAME mirador.cache.capability_cache
    SOURCES capability_cache_test.cpp
    LIBS mirador::cache
    LABELS unit)

mirador_add_test(
    NAME mirador.cache.visual_index
    SOURCES visual_index_test.cpp
    LIBS mirador::cache
    LABELS unit)
