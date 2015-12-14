include_directories("${winterdir}")

set(winter
"${winterdir}/BuiltInFunctionImpl.cpp"
"${winterdir}/LanguageTests.cpp"
"${winterdir}/Linker.cpp"
"${winterdir}/LLVMTypeUtils.cpp"
"${winterdir}/TokenBase.cpp"
"${winterdir}/Value.cpp"
"${winterdir}/VirtualMachine.cpp"
"${winterdir}/wnt_ASTNode.cpp"
"${winterdir}/wnt_Diagnostics.cpp"
"${winterdir}/wnt_ExternalFunction.cpp"
"${winterdir}/wnt_Frame.cpp"
"${winterdir}/wnt_FunctionDefinition.cpp"
"${winterdir}/wnt_FunctionExpression.cpp"
"${winterdir}/wnt_FunctionSignature.cpp"
"${winterdir}/wnt_LangParser.cpp"
"${winterdir}/wnt_Lexer.cpp"
"${winterdir}/wnt_SourceBuffer.cpp"
"${winterdir}/wnt_Type.cpp"
"${winterdir}/wnt_RefCounting.cpp"
"${winterdir}/ProofUtils.cpp"
"${winterdir}/wnt_ArrayLiteral.cpp"
"${winterdir}/wnt_TupleLiteral.cpp"
"${winterdir}/wnt_VectorLiteral.cpp"
"${winterdir}/wnt_VArrayLiteral.cpp"
"${winterdir}/wnt_Variable.cpp"
"${winterdir}/FuzzTests.cpp"
"${winterdir}/BaseException.h"
"${winterdir}/BuiltInFunctionImp.h"
"${winterdir}/BuiltInFunctionImpl.h"
"${winterdir}/GeneratedTokens.h"
"${winterdir}/LanguageTests.h"
"${winterdir}/Linker.h"
"${winterdir}/LLVMTypeUtils.h"
"${winterdir}/TokenBase.h"
"${winterdir}/Value.h"
"${winterdir}/VirtualMachine.h"
"${winterdir}/VMState.h"
"${winterdir}/wnt_ASTNode.h"
"${winterdir}/wnt_Diagnostics.h"
"${winterdir}/wnt_ExternalFunction.h"
"${winterdir}/wnt_Frame.h"
"${winterdir}/wnt_FunctionDefinition.h"
"${winterdir}/wnt_FunctionExpression.h"
"${winterdir}/wnt_FunctionSignature.h"
"${winterdir}/wnt_LangParser.h"
"${winterdir}/wnt_Lexer.h"
"${winterdir}/wnt_SourceBuffer.h"
"${winterdir}/wnt_Type.h"
"${winterdir}/wnt_RefCounting.h"
"${winterdir}/ProofUtils.h"
"${winterdir}/wnt_IfExpression.cpp"
"${winterdir}/wnt_IfExpression.h"
"${winterdir}/wnt_LLVMVersion.h"
"${winterdir}/wnt_ArrayLiteral.h"
"${winterdir}/wnt_TupleLiteral.h"
"${winterdir}/wnt_VectorLiteral.h"
"${winterdir}/wnt_VArrayLiteral.h"
"${winterdir}/wnt_Variable.h"
"${winterdir}/FuzzTests.h"
)

SOURCE_GROUP(winter FILES ${winter})