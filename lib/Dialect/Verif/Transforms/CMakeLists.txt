add_circt_dialect_library(CIRCTVerifTransforms
  CombineAssertLike.cpp
  FoldAssume.cpp
  LowerContracts.cpp
  LowerSymbolicValues.cpp
  LowerTests.cpp
  PrepareForFormal.cpp
  SimplifyAssumeEq.cpp
  StripContracts.cpp
  SimplifyODC.cpp
  VerifyClockedAssertLike.cpp

  DEPENDS
  CIRCTVerifTransformsIncGen

  LINK_LIBS PUBLIC
  CIRCTComb
  CIRCTHW
  CIRCTLTL
  CIRCTSupport
  CIRCTSV
  CIRCTVerif
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRTransformUtils
)
