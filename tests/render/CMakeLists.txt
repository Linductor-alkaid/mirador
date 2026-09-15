# Unit tests for mirador::render (M4-06). The render module links exactly
# mirador::fusion (locked by tests/architecture), so the targets below pull in
# the fusion API transitively only.

mirador_add_test(
    NAME mirador.render.set_of_mark
    SOURCES set_of_mark_test.cpp
    LIBS mirador::render
    LABELS unit)

mirador_add_test(
    NAME mirador.render.grid_partition
    SOURCES grid_partition_test.cpp
    LIBS mirador::render
    LABELS unit)
