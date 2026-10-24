# The vendored dav1d (lib/dav1d, BSD-2-Clause) as an ESP-IDF component, so the
# T-Display P4 decodes MeshCore Open TAK photos (VoxAv1Core: <dav1d/dav1d.h>
# found -> VOX_AV1_AVAILABLE). Same sources and flags as lib/dav1d/library.json
# and fast_paths.py: everything at -Os, the few files a decode spends its time
# in at -O2. Portable C, no assembly (gen/config.h).
set(DAV1D "${CMAKE_CURRENT_LIST_DIR}/../../../lib/dav1d")

file(GLOB DAV1D_SRCS "${DAV1D}/src/*.c" "${DAV1D}/src/8bpc/*.c")
list(FILTER DAV1D_SRCS EXCLUDE REGEX "_tmpl\\.c$")

idf_component_register(
  SRCS ${DAV1D_SRCS}
  INCLUDE_DIRS "${DAV1D}/include"
  PRIV_INCLUDE_DIRS "${DAV1D}/gen" "${DAV1D}"
)

# Third-party code we don't own: never -Werror-fail on it.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -Os)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG)

set(DAV1D_HOT
  "${DAV1D}/src/msac.c"
  "${DAV1D}/src/8bpc/recon_8.c"
  "${DAV1D}/src/8bpc/mc_8.c"
  "${DAV1D}/src/8bpc/itx_8.c"
  "${DAV1D}/src/8bpc/ipred_8.c"
  "${DAV1D}/src/8bpc/ipred_prepare_8.c"
  "${DAV1D}/src/8bpc/cdef_8.c"
  "${DAV1D}/src/8bpc/cdef_apply_8.c"
  "${DAV1D}/src/8bpc/loopfilter_8.c"
  "${DAV1D}/src/8bpc/lf_apply_8.c"
  "${DAV1D}/src/8bpc/looprestoration_8.c")
set_source_files_properties(${DAV1D_HOT} PROPERTIES COMPILE_OPTIONS "-O2")
