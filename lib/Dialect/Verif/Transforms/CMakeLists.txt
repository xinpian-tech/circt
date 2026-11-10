add_circt_dialect_library(CIRCTVerifTransforms
  AssertionsToOutput.cpp
  AssumeInputs.cpp
  CombineAssertLike.cpp
  FoldAssume.cpp
  LowerContracts.cpp
  LowerSymbolicValues.cpp
  LowerTests.cpp
  PrepareForFormal.cpp
  ShiftTarget.cpp
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
  CIRCTSynth
  CIRCTVerif
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRTransformUtils
)
