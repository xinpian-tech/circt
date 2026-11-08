add_circt_dialect_library(CIRCTMooreTransforms
SimplifyRefs.cpp
SimplifyProcedures.cpp
CreateVTables.cpp
ResolveXMR.cpp

  DEPENDS
  CIRCTMooreTransformsIncGen

  LINK_LIBS PUBLIC
  CIRCTMoore
  CIRCTSupport
  MLIRIR
  MLIRPass
  MLIRTransformUtils
)
