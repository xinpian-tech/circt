add_circt_dialect_library(CIRCTVerifTransforms
  AssertionsToOutput.cpp
  CombineAssertLike.cpp
  FoldAssume.cpp
  LowerContracts.cpp
  LowerSymbolicValues.cpp
  LowerTests.cpp
  AssumeInputs.cpp
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
  CIRCTSeq
  CIRCTSupport
  CIRCTSV
  CIRCTVerif
  CIRCTSynth
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRTransformUtils
)
