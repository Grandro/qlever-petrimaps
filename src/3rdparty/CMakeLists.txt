file(GLOB_RECURSE 3RDPARTY_SRC *.c *.cpp)

add_library(3rdparty_dep ${3RDPARTY_SRC})
