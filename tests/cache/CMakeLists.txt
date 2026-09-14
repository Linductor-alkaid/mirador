# Unit tests for mirador::cache public contracts (M1-07).

mirador_add_test(
    NAME mirador.cache.frame_cache
    SOURCES frame_cache_test.cpp
    LIBS mirador::cache
    LABELS unit)
