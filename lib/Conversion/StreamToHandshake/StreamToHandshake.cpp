//===- StreamToHandshake.cpp - Translate Stream into Handshake ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===----------------------------------------------------------------------===//
//
// This is the main Stream to Handshake Conversion Pass Implementation.
//
//===----------------------------------------------------------------------===//

#include "circt-stream/Conversion/StreamToHandshake.h"

#include <llvm/ADT/STLExtras.h>

#include "../PassDetail.h"
#include "circt-stream/Dialect/Stream/StreamDialect.h"
#include "circt-stream/Dialect/Stream/StreamOps.h"
#include "circt/Conversion/StandardToHandshake.h"
#include "circt/Dialect/Handshake/HandshakeDialect.h"
#include "circt/Dialect/Handshake/HandshakeOps.h"
#include "circt/Support/SymCache.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/FormatVariadic.h"

using namespace circt;
using namespace circt::handshake;
using namespace mlir;
using namespace circt_stream;
using namespace circt_stream::stream;

namespace {

/// Returns a name resulting from an operation, without discriminating
/// type information.
static std::string getBareOpName(Operation *op) {
  std::string name = op->getName().getStringRef().str();
  std::replace(name.begin(), name.end(), '.', '_');
  return name;
}

/// Helper class that provides functionality for creating unique symbol names.
/// One instance is shared among all patterns.
/// The uniquer remembers all symbols and new ones by checking that they do not
/// exist yet.
class SymbolUniquer {
public:
  SymbolUniquer(Operation *top) : context(top->getContext()) {
    addDefinitions(top);
  }

  void addDefinitions(mlir::Operation *top) {
    for (auto &region : top->getRegions())
      for (auto &block : region.getBlocks())
        for (auto symOp : block.getOps<mlir::SymbolOpInterface>())
          addSymbol(symOp.getName().str());
  }

  std::string getUniqueSymName(Operation *op) {
    std::string opName = getBareOpName(op);
    std::string name = opName;

    unsigned cnt = 1;
    while (usedNames.contains(name)) {
      name = llvm::formatv("{0}_{1}", opName, cnt++);
    }
    addSymbol(name);

    return name;
  }

  void addSymbol(std::string name) { usedNames.insert(name); }

private:
  MLIRContext *context;
  llvm::SmallSet<std::string, 8> usedNames;
};

class StreamTypeConverter : public TypeConverter {
public:
  StreamTypeConverter() {
    addConversion([](Type type) { return type; });
    addConversion([](StreamType type, SmallVectorImpl<Type> &res) {
      MLIRContext *ctx = type.getContext();
      res.push_back(TupleType::get(
          ctx, {type.getElementType(), IntegerType::get(ctx, 1)}));
      res.push_back(NoneType::get(ctx));
      return success();
    });
  }
};

// Functionality to share state when lowering, see CIRCT's HandshakeLowering
class StreamLowering : public HandshakeLowering {
public:
  explicit StreamLowering(Region &r) : HandshakeLowering(r) {}
};

struct FuncOpLowering : public OpConversionPattern<func::FuncOp> {
  using OpConversionPattern<func::FuncOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(func::FuncOp op, OpAdaptor adapter,
                  ConversionPatternRewriter &rewriter) const override {
    // type conversion
    TypeConverter *typeConverter = getTypeConverter();
    FunctionType oldFuncType = op.getFunctionType().cast<FunctionType>();

    TypeConverter::SignatureConversion sig(oldFuncType.getNumInputs());
    SmallVector<Type> newResults;
    if (failed(
            typeConverter->convertSignatureArgs(oldFuncType.getInputs(), sig)))
      return failure();

    // Add ctrl signal for initialization control flow
    sig.addInputs(rewriter.getNoneType());

    if (failed(typeConverter->convertTypes(oldFuncType.getResults(),
                                           newResults)) ||
        failed(
            rewriter.convertRegionTypes(&op.getBody(), *typeConverter, &sig)))
      return failure();

    // Add ctrl
    newResults.push_back(rewriter.getNoneType());

    auto newFuncType =
        rewriter.getFunctionType(sig.getConvertedTypes(), newResults);

    SmallVector<NamedAttribute, 4> attributes;
    for (const auto &attr : op->getAttrs()) {
      if (attr.getName() == SymbolTable::getSymbolAttrName() ||
          attr.getName() == function_interface_impl::getTypeAttrName())
        continue;
      attributes.push_back(attr);
    }

    auto newFuncOp = rewriter.create<handshake::FuncOp>(
        op.getLoc(), op.getName(), newFuncType, attributes);
    rewriter.inlineRegionBefore(op.getBody(), newFuncOp.getBody(),
                                newFuncOp.end());

    rewriter.eraseOp(op);
    newFuncOp.resolveArgAndResNames();

    return success();
  }
};

static Value getBlockCtrlSignal(Block *block) {
  Value ctrl = block->getArguments().back();
  assert(ctrl.getType().isa<NoneType>() &&
         "last argument should be a ctrl signal");
  return ctrl;
}

// TODO: this function require strong assumptions. Relax this if possible
// Assumes that the op producing the input data also produces a ctrl signal
static Value getCtrlSignal(ValueRange operands) {
  assert(operands.size() > 0);
  Value fstOp = operands.front();
  if (BlockArgument arg = fstOp.dyn_cast_or_null<BlockArgument>()) {
    return getBlockCtrlSignal(arg.getOwner());
  }
  Operation *op = fstOp.getDefiningOp();
  if (isa<handshake::InstanceOp>(op)) {
    return op->getResults().back();
  }

  return getCtrlSignal(op->getOperands());
}

static void resolveStreamOperand(Value oldOperand,
                                 SmallVectorImpl<Value> &newOperands) {
  assert(oldOperand.getType().isa<StreamType>());
  // TODO: is there another way to resolve this directly?
  auto castOp =
      dyn_cast<UnrealizedConversionCastOp>(oldOperand.getDefiningOp());
  for (auto castOperand : castOp.getInputs())
    newOperands.push_back(castOperand);
}

static void resolveNewOperands(Operation *oldOperation,
                               ValueRange remappedOperands,
                               SmallVectorImpl<Value> &newOperands) {
  for (auto [oldOp, remappedOp] :
       llvm::zip(oldOperation->getOperands(), remappedOperands))
    resolveStreamOperand(remappedOp, newOperands);

  // Resolve the init ctrl signal
  if (remappedOperands.size() == 0) {
    Value ctrl = oldOperation->getBlock()->getArguments().back();
    newOperands.push_back(ctrl);
  } else {
    newOperands.push_back(getCtrlSignal(remappedOperands));
  }
}

struct ReturnOpLowering : public OpConversionPattern<func::ReturnOp> {
  using OpConversionPattern<func::ReturnOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(func::ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.replaceOpWithNewOp<handshake::ReturnOp>(op, operands);
    return success();
  }
};

static Block *getTopLevelBlock(Operation *op) {
  return &op->getParentOfType<ModuleOp>().getRegion().front();
}

/// Creates a new FuncOp that encapsulates the provided region.
static FuncOp createFuncOp(Region &region, StringRef name, TypeRange argTypes,
                           TypeRange resTypes,
                           ConversionPatternRewriter &rewriter) {
  auto func_type = rewriter.getFunctionType(argTypes, resTypes);
  FuncOp newFuncOp =
      rewriter.create<FuncOp>(rewriter.getUnknownLoc(), name, func_type);

  // Makes the function private
  SymbolTable::setSymbolVisibility(newFuncOp, SymbolTable::Visibility::Private);

  rewriter.inlineRegionBefore(region, newFuncOp.getBody(), newFuncOp.end());
  newFuncOp.resolveArgAndResNames();
  assert(newFuncOp.getRegion().hasOneBlock() &&
         "expected std to handshake to produce region with one block");

  return newFuncOp;
}

/// Replaces op with a new InstanceOp that calls the provided function.
static InstanceOp replaceWithInstance(Operation *op, FuncOp func,
                                      ValueRange newOperands,
                                      ConversionPatternRewriter &rewriter) {
  rewriter.setInsertionPoint(op);
  InstanceOp instance =
      rewriter.create<InstanceOp>(op->getLoc(), func, newOperands);

  SmallVector<Value> newValues;
  auto resultIt = instance->getResults().begin();
  for (auto oldResType : op->getResultTypes()) {
    assert(oldResType.isa<StreamType>() &&
           "can currently only replace stream types");

    // TODO this is very fragile
    auto tuple = *resultIt++;
    auto ctrl = *resultIt++;

    auto castOp = rewriter.create<UnrealizedConversionCastOp>(
        op->getLoc(), oldResType, ValueRange({tuple, ctrl}));
    newValues.push_back(castOp.getResult(0));
  }
  rewriter.replaceOp(op, newValues);

  return instance;
}

// Usual flow:
// 1. Apply lowerRegion from StdToHandshake
// 2. Collect operands
// 3. Create new signature
// 4. Apply signature changes
// 5. Change parts of the lowered Region to fit the operations needs.
// 6. Create function and replace operation with InstanceOp

template <typename Op>
struct StreamOpLowering : public OpConversionPattern<Op> {
  StreamOpLowering(TypeConverter &typeConverter, MLIRContext *ctx,
                   SymbolUniquer &symbolUniquer)
      : OpConversionPattern<Op>(typeConverter, ctx),
        symbolUniquer(symbolUniquer) {}

  SymbolUniquer &symbolUniquer;
};

// Builds a handshake::FuncOp and that represents the mapping funtion. This
// function is then instantiated and connected to its inputs and outputs.
struct MapOpLowering : public StreamOpLowering<MapOp> {
  using StreamOpLowering::StreamOpLowering;

  LogicalResult
  matchAndRewrite(MapOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    TypeConverter *typeConverter = getTypeConverter();

    // create surrounding region
    Region r;

    SmallVector<Type> inputTypes;
    if (failed(typeConverter->convertTypes(op->getOperandTypes(), inputTypes)))
      return failure();
    inputTypes.push_back(rewriter.getNoneType());

    SmallVector<Location> argLocs(inputTypes.size(), loc);

    Block *entryBlock =
        rewriter.createBlock(&r, r.begin(), inputTypes, argLocs);
    Value tupleIn = entryBlock->getArgument(0);
    Value streamCtrl = entryBlock->getArgument(1);
    Value initCtrl = entryBlock->getArgument(2);

    auto unpack = rewriter.create<handshake::UnpackOp>(loc, tupleIn);
    Value data = unpack.getResult(0);
    Value eos = unpack.getResult(1);

    Block *lambda = &op.getRegion().front();
    rewriter.mergeBlocks(lambda, entryBlock, ValueRange({data, streamCtrl}));

    Operation *oldTerm = entryBlock->getTerminator();

    rewriter.setInsertionPoint(oldTerm);
    auto tupleOut = rewriter.create<handshake::PackOp>(
        oldTerm->getLoc(), ValueRange({oldTerm->getOperand(0), eos}));

    SmallVector<Value> newTermOperands = {tupleOut, oldTerm->getOperand(1),
                                          initCtrl};
    auto newTerm = rewriter.replaceOpWithNewOp<handshake::ReturnOp>(
        oldTerm, newTermOperands);

    TypeRange resTypes = newTerm->getOperandTypes();

    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    FuncOp newFuncOp =
        createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                     entryBlock->getArgumentTypes(), resTypes, rewriter);

    replaceWithInstance(op, newFuncOp, operands, rewriter);

    return success();
  }
};

struct FilterOpLowering : public StreamOpLowering<FilterOp> {
  using StreamOpLowering::StreamOpLowering;

  LogicalResult
  matchAndRewrite(FilterOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    TypeConverter *typeConverter = getTypeConverter();

    Region r;

    SmallVector<Type> inputTypes;
    if (failed(typeConverter->convertTypes(op->getOperandTypes(), inputTypes)))
      return failure();
    inputTypes.push_back(rewriter.getNoneType());

    SmallVector<Location> argLocs(inputTypes.size(), loc);

    Block *entryBlock =
        rewriter.createBlock(&r, r.begin(), inputTypes, argLocs);
    Value tupleIn = entryBlock->getArgument(0);
    Value streamCtrl = entryBlock->getArgument(1);
    Value initCtrl = entryBlock->getArgument(2);

    auto unpack = rewriter.create<handshake::UnpackOp>(loc, tupleIn);
    Value data = unpack.getResult(0);
    Value eos = unpack.getResult(1);

    Block *lambda = &op.getRegion().front();
    rewriter.mergeBlocks(lambda, entryBlock, ValueRange({data, streamCtrl}));

    Operation *oldTerm = entryBlock->getTerminator();

    assert(oldTerm->getNumOperands() == 2 &&
           "expected handshake::ReturnOp to have two operands");
    rewriter.setInsertionPointToEnd(entryBlock);

    Value cond = oldTerm->getOperand(0);
    Value ctrl = oldTerm->getOperand(1);

    auto tupleOut =
        rewriter.create<handshake::PackOp>(loc, ValueRange({data, eos}));

    auto condOrEos = rewriter.create<arith::OrIOp>(loc, cond, eos);

    auto dataBr = rewriter.create<handshake::ConditionalBranchOp>(
        rewriter.getUnknownLoc(), condOrEos, tupleOut);

    // Makes sure we only emit Ctrl when data is produced
    auto ctrlBr = rewriter.create<handshake::ConditionalBranchOp>(
        rewriter.getUnknownLoc(), condOrEos, ctrl);

    SmallVector<Value> newTermOperands = {dataBr.trueResult(),
                                          ctrlBr.trueResult(), initCtrl};
    auto newTerm = rewriter.replaceOpWithNewOp<handshake::ReturnOp>(
        oldTerm, newTermOperands);

    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    FuncOp newFuncOp = createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                                    entryBlock->getArgumentTypes(),
                                    newTerm.getOperandTypes(), rewriter);
    replaceWithInstance(op, newFuncOp, operands, rewriter);
    return success();
  }
};

/// Lowers a reduce operation to a ahndshake circuit
///
/// Accumulates the result of the reduction in a buffer. On EOS this result is
/// emitted, followed by a EOS = true one cycle after the emission of the
/// result.
///
/// While the reduction is running, no output is produced.
struct ReduceOpLowering : public StreamOpLowering<ReduceOp> {
  using StreamOpLowering::StreamOpLowering;

  LogicalResult
  matchAndRewrite(ReduceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();

    TypeConverter *typeConverter = getTypeConverter();
    SmallVector<Type> resultTypes;
    if (failed(typeConverter->convertType(op.result().getType(), resultTypes)))
      return failure();

    assert(resultTypes[0].isa<TupleType>());
    Type resultType = resultTypes[0].dyn_cast<TupleType>().getType(0);

    // TODO: handshake currently only supports i64 buffers, change this as
    // soon as support for other types is added.
    assert(resultType == rewriter.getI64Type() &&
           "currently, only i64 buffers are supported");

    Region r;

    SmallVector<Type> inputTypes;
    if (failed(typeConverter->convertTypes(op->getOperandTypes(), inputTypes)))
      return failure();
    inputTypes.push_back(rewriter.getNoneType());

    SmallVector<Location> argLocs(inputTypes.size(), loc);

    Block *entryBlock =
        rewriter.createBlock(&r, r.begin(), inputTypes, argLocs);
    Value tupleIn = entryBlock->getArgument(0);
    Value streamCtrl = entryBlock->getArgument(1);
    Value initCtrl = entryBlock->getArgument(2);

    auto unpack = rewriter.create<handshake::UnpackOp>(loc, tupleIn);
    Value data = unpack.getResult(0);
    Value eos = unpack.getResult(1);

    Block *lambda = &op.getRegion().front();

    Operation *oldTerm = lambda->getTerminator();
    auto buffer = rewriter.create<handshake::BufferOp>(
        rewriter.getUnknownLoc(), resultType, 1, oldTerm->getOperand(0),
        BufferTypeEnum::seq);
    // This does return an unsigned integer but expects signed integers
    // TODO check if this is an MLIR bug
    buffer->setAttr("initValues",
                    rewriter.getI64ArrayAttr({(int64_t)adaptor.initValue()}));

    auto dataBr = rewriter.create<handshake::ConditionalBranchOp>(
        rewriter.getUnknownLoc(), eos, buffer);
    auto eosBr = rewriter.create<handshake::ConditionalBranchOp>(
        rewriter.getUnknownLoc(), eos, eos);
    auto ctrlBr = rewriter.create<handshake::ConditionalBranchOp>(
        rewriter.getUnknownLoc(), eos, oldTerm->getOperand(1));

    rewriter.mergeBlocks(lambda, entryBlock,
                         ValueRange({dataBr.falseResult(), data, streamCtrl}));

    rewriter.setInsertionPoint(oldTerm);

    // Connect outputs and ensure correct delay between value and EOS=true
    // emission A sequental buffer ensures a cycle delay of 1
    auto eosFalse = rewriter.create<handshake::ConstantOp>(
        rewriter.getUnknownLoc(),
        rewriter.getIntegerAttr(rewriter.getI1Type(), 0), ctrlBr.trueResult());
    auto tupleOutVal = rewriter.create<handshake::PackOp>(
        loc, ValueRange({dataBr.trueResult(), eosFalse}));

    auto tupleOutEOS = rewriter.create<handshake::PackOp>(
        loc, ValueRange({dataBr.trueResult(), eosBr.trueResult()}));

    // Not really needed, but the BufferOp builder requires an input
    auto bubble = rewriter.create<ConstantOp>(
        loc, rewriter.getIntegerAttr(rewriter.getI1Type(), 0),
        ctrlBr.trueResult());
    auto select = rewriter.create<handshake::BufferOp>(
        rewriter.getUnknownLoc(), rewriter.getI32Type(), 2, bubble,
        BufferTypeEnum::seq);
    // First select the tupleOut, afterwards the one with the EOS signal
    select->setAttr("initValues", rewriter.getI64ArrayAttr({1, 0}));

    auto tupleOut = rewriter.create<MuxOp>(
        loc, select, ValueRange({tupleOutVal, tupleOutEOS}));
    auto ctrlOut = rewriter.create<MuxOp>(
        loc, select, ValueRange({ctrlBr.trueResult(), ctrlBr.trueResult()}));

    SmallVector<Value> newTermOperands = {tupleOut, ctrlOut, initCtrl};

    auto newTerm = rewriter.replaceOpWithNewOp<handshake::ReturnOp>(
        oldTerm, newTermOperands);

    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    FuncOp newFuncOp = createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                                    entryBlock->getArgumentTypes(),
                                    newTerm.getOperandTypes(), rewriter);

    replaceWithInstance(op, newFuncOp, operands, rewriter);
    return success();
  }
};

struct PackOpLowering : public OpConversionPattern<stream::PackOp> {
  using OpConversionPattern<stream::PackOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(stream::PackOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<handshake::PackOp>(op, adaptor.getOperands());

    return success();
  }
};

struct UnpackOpLowering : public OpConversionPattern<stream::UnpackOp> {
  using OpConversionPattern<stream::UnpackOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(stream::UnpackOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<handshake::UnpackOp>(op, adaptor.input());

    return success();
  }
};

struct CreateOpLowering : public StreamOpLowering<CreateOp> {
  using StreamOpLowering::StreamOpLowering;

  // TODO add location usage
  LogicalResult
  matchAndRewrite(stream::CreateOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Region r;
    Location loc = op.getLoc();

    Block *entryBlock = rewriter.createBlock(&r, {}, {rewriter.getNoneType()},
                                             {rewriter.getUnknownLoc()});

    // TODO ensure that subsequent ctrl inputs are ignored
    Value ctrlIn = entryBlock->getArgument(0);
    size_t bufSize = op.values().size();
    Type elementType = op.getElementType();
    assert(elementType.isa<IntegerType>());

    rewriter.setInsertionPointToEnd(entryBlock);

    // Only use in ctrl once
    auto falseVal = rewriter.create<handshake::ConstantOp>(
        rewriter.getUnknownLoc(),
        rewriter.getIntegerAttr(rewriter.getI1Type(), 0), ctrlIn);
    auto fst = rewriter.create<BufferOp>(loc, rewriter.getI1Type(), 1, falseVal,
                                         BufferTypeEnum::seq);
    fst->setAttr("initValues", rewriter.getI64ArrayAttr({1}));
    auto useCtrl =
        rewriter.create<handshake::ConditionalBranchOp>(loc, fst, ctrlIn);

    // Ctrl "looping" and selection
    // We have to change the input later on
    auto tmpCtrl = rewriter.create<NeverOp>(loc, rewriter.getNoneType());
    auto ctrlBuf = rewriter.create<BufferOp>(loc, rewriter.getNoneType(), 1,
                                             tmpCtrl, BufferTypeEnum::seq);
    auto ctrl = rewriter.create<MergeOp>(
        loc, ValueRange({useCtrl.trueResult(), ctrlBuf}));
    rewriter.replaceOp(tmpCtrl, {ctrl});

    // Data part

    auto bubble = rewriter.create<handshake::ConstantOp>(
        loc, rewriter.getIntegerAttr(elementType, 0), ctrl);
    auto dataBuf = rewriter.create<BufferOp>(loc, elementType, bufSize, bubble,
                                             BufferTypeEnum::seq);
    // The buffer works in reverse
    SmallVector<int64_t> values;
    for (auto attr : llvm::reverse(op.values())) {
      assert(attr.isa<IntegerAttr>());
      values.push_back(attr.dyn_cast<IntegerAttr>().getInt());
    }
    dataBuf->setAttr("initValues", rewriter.getI64ArrayAttr(values));
    auto cnt = rewriter.create<BufferOp>(loc, rewriter.getI64Type(), 1, bubble,
                                         BufferTypeEnum::seq);
    // initialize cnt to 0 to indicate that 0 elements were emitted
    cnt->setAttr("initValues", rewriter.getI64ArrayAttr({0}));

    auto one = rewriter.create<handshake::ConstantOp>(
        loc, rewriter.getIntegerAttr(rewriter.getI64Type(), 1), ctrl);

    auto sizeConst = rewriter.create<handshake::ConstantOp>(
        loc, rewriter.getIntegerAttr(rewriter.getI64Type(), bufSize), ctrl);

    auto finished = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, cnt, sizeConst);

    auto newCnt = rewriter.create<arith::AddIOp>(op.getLoc(), cnt, one);
    // ensure looping of cnt
    cnt.setOperand(newCnt);

    auto tupleOut = rewriter.create<handshake::PackOp>(
        loc, ValueRange({dataBuf, finished}));

    // create terminator
    auto term = rewriter.create<handshake::ReturnOp>(
        loc, ValueRange({tupleOut.result(), ctrl}));

    // Collect types of function
    SmallVector<Type> argTypes;
    argTypes.push_back(rewriter.getNoneType());

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    auto newFuncOp = createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                                  argTypes, term.getOperandTypes(), rewriter);

    replaceWithInstance(op, newFuncOp, {getBlockCtrlSignal(op->getBlock())},
                        rewriter);
    return success();
  }
};

struct SplitOpLowering : public StreamOpLowering<SplitOp> {
  using StreamOpLowering::StreamOpLowering;

  LogicalResult
  matchAndRewrite(SplitOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    TypeConverter *typeConverter = getTypeConverter();

    // create surrounding region
    Region r;

    SmallVector<Type> inputTypes;
    if (failed(typeConverter->convertTypes(op->getOperandTypes(), inputTypes)))
      return failure();
    inputTypes.push_back(rewriter.getNoneType());

    SmallVector<Location> argLocs(inputTypes.size(), loc);

    Block *entryBlock =
        rewriter.createBlock(&r, r.begin(), inputTypes, argLocs);
    Value tupleIn = entryBlock->getArgument(0);
    Value streamCtrl = entryBlock->getArgument(1);
    Value initCtrl = entryBlock->getArgument(2);

    auto unpack = rewriter.create<handshake::UnpackOp>(loc, tupleIn);
    Value data = unpack.getResult(0);
    Value eos = unpack.getResult(1);

    Block *lambda = &op.getRegion().front();
    rewriter.mergeBlocks(lambda, entryBlock, ValueRange({data, streamCtrl}));

    Operation *oldTerm = entryBlock->getTerminator();

    rewriter.setInsertionPoint(oldTerm);
    SmallVector<Value> newTermOperands;
    for (auto oldOp : oldTerm->getOperands().drop_back()) {
      auto pack = rewriter.create<handshake::PackOp>(oldTerm->getLoc(),
                                                     ValueRange({oldOp, eos}));
      newTermOperands.push_back(pack.getResult());
      newTermOperands.push_back(oldTerm->getOperands().back());
    }

    newTermOperands.push_back(initCtrl);
    auto newTerm = rewriter.replaceOpWithNewOp<handshake::ReturnOp>(
        oldTerm, newTermOperands);

    TypeRange resTypes = newTerm->getOperandTypes();

    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    FuncOp newFuncOp =
        createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                     entryBlock->getArgumentTypes(), resTypes, rewriter);

    replaceWithInstance(op, newFuncOp, operands, rewriter);

    return success();
  }
};

/// TODO: make this more efficient
template <typename Op>
static Value buildReduceTree(ValueRange values, Location loc,
                             ConversionPatternRewriter &rewriter) {
  assert(values.size() > 0);
  Value res = values.front();
  for (auto val : values.drop_front()) {
    res = rewriter.create<Op>(loc, res, val);
  }
  return res;
}

struct CombineOpLowering : public StreamOpLowering<CombineOp> {
  using StreamOpLowering::StreamOpLowering;

  LogicalResult
  matchAndRewrite(CombineOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    TypeConverter *typeConverter = getTypeConverter();

    // create surrounding region
    Region r;

    SmallVector<Type> inputTypes;
    if (failed(typeConverter->convertTypes(op->getOperandTypes(), inputTypes)))
      return failure();
    inputTypes.push_back(rewriter.getNoneType());

    SmallVector<Location> argLocs(inputTypes.size(), loc);

    Block *entryBlock =
        rewriter.createBlock(&r, r.begin(), inputTypes, argLocs);

    SmallVector<Value> blockInputs;
    SmallVector<Value> eosInputs;
    SmallVector<Value> ctrlInputs;
    for (unsigned i = 0, e = entryBlock->getNumArguments() - 1; i < e; i += 2) {
      Value tupleIn = entryBlock->getArgument(i);
      Value streamCtrl = entryBlock->getArgument(i + 1);
      auto unpack = rewriter.create<handshake::UnpackOp>(loc, tupleIn);
      Value data = unpack.getResult(0);
      Value eos = unpack.getResult(1);

      blockInputs.push_back(data);
      ctrlInputs.push_back(streamCtrl);
      eosInputs.push_back(eos);
    }
    Value initCtrl = entryBlock->getArguments().back();

    // only execute region when ALL inputs are ready
    auto ctrlJoin = rewriter.create<JoinOp>(loc, ctrlInputs);
    blockInputs.push_back(ctrlJoin);
    Block *lambda = &op.getRegion().front();

    rewriter.mergeBlocks(lambda, entryBlock, blockInputs);

    Operation *oldTerm = entryBlock->getTerminator();
    rewriter.setInsertionPoint(oldTerm);

    // TODO What to do when not all streams provide an eos signal
    Value eos = buildReduceTree<arith::OrIOp>(eosInputs, loc, rewriter);

    SmallVector<Value> newTermOperands;
    for (auto oldOp : oldTerm->getOperands().drop_back()) {
      auto pack = rewriter.create<handshake::PackOp>(oldTerm->getLoc(),
                                                     ValueRange({oldOp, eos}));
      newTermOperands.push_back(pack.getResult());
      newTermOperands.push_back(oldTerm->getOperands().back());
    }

    newTermOperands.push_back(initCtrl);
    auto newTerm = rewriter.replaceOpWithNewOp<handshake::ReturnOp>(
        oldTerm, newTermOperands);

    TypeRange resTypes = newTerm->getOperandTypes();

    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    FuncOp newFuncOp =
        createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                     entryBlock->getArgumentTypes(), resTypes, rewriter);

    replaceWithInstance(op, newFuncOp, operands, rewriter);

    return success();
  }
};

struct SinkOpLowering : public StreamOpLowering<stream::SinkOp> {
  using StreamOpLowering::StreamOpLowering;

  LogicalResult
  matchAndRewrite(stream::SinkOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    TypeConverter *typeConverter = getTypeConverter();

    Region r;

    SmallVector<Type> inputTypes;
    if (failed(typeConverter->convertTypes(op->getOperandTypes(), inputTypes)))
      return failure();
    inputTypes.push_back(rewriter.getNoneType());

    SmallVector<Location> argLocs(inputTypes.size(), loc);

    Block *entryBlock =
        rewriter.createBlock(&r, r.begin(), inputTypes, argLocs);

    // Don't use the values to ensure that handshake::SinkOps will be inserted
    Value initCtrl = entryBlock->getArguments().back();
    auto newTerm =
        rewriter.create<handshake::ReturnOp>(loc, ValueRange(initCtrl));

    TypeRange resTypes = newTerm->getOperandTypes();

    SmallVector<Value> operands;
    resolveNewOperands(op, adaptor.getOperands(), operands);

    rewriter.setInsertionPointToStart(getTopLevelBlock(op));
    FuncOp newFuncOp =
        createFuncOp(r, symbolUniquer.getUniqueSymName(op),
                     entryBlock->getArgumentTypes(), resTypes, rewriter);

    replaceWithInstance(op, newFuncOp, operands, rewriter);

    return success();
  }
};

static void
populateStreamToHandshakePatterns(StreamTypeConverter &typeConverter,
                                  SymbolUniquer symbolUniquer,
                                  RewritePatternSet &patterns) {
  // clang-format off
  patterns.add<
    FuncOpLowering,
    ReturnOpLowering,
    PackOpLowering,
    UnpackOpLowering
  >(typeConverter, patterns.getContext());

  patterns.add<
    MapOpLowering,
    FilterOpLowering,
    ReduceOpLowering,
    CreateOpLowering,
    SplitOpLowering,
    CombineOpLowering,
    SinkOpLowering
  >(typeConverter, patterns.getContext(), symbolUniquer);
  // clang-format on
}

// ensures that the IR is in a valid state after the initial partial
// conversion
static LogicalResult materializeForksAndSinks(ModuleOp m) {
  for (auto funcOp :
       llvm::make_early_inc_range(m.getOps<handshake::FuncOp>())) {
    OpBuilder builder(funcOp);
    if (addForkOps(funcOp.getRegion(), builder).failed() ||
        addSinkOps(funcOp.getRegion(), builder).failed() ||
        verifyAllValuesHasOneUse(funcOp).failed())
      return failure();
  }

  return success();
}

/// Removes all forks and syncs as the insertion is not able to extend existing
/// forks
static LogicalResult dematerializeForksAndSinks(Region &r) {
  for (auto sinkOp : llvm::make_early_inc_range(r.getOps<handshake::SinkOp>()))
    sinkOp.erase();

  for (auto forkOp :
       llvm::make_early_inc_range(r.getOps<handshake::ForkOp>())) {
    for (auto res : forkOp->getResults())
      res.replaceAllUsesWith(forkOp.getOperand());
    forkOp.erase();
  }
  return success();
}

// TODO Do this with an op trait?
bool isStreamOp(Operation *op) {
  return isa<MapOp, FilterOp, ReduceOp, SplitOp, CombineOp>(op);
}

/// Traverses the modules region recursively and applies the std to handshake
/// conversion on each stream operation region.
LogicalResult transformStdRegions(ModuleOp m) {
  // go over all stream ops and transform their regions
  for (auto funcOp : llvm::make_early_inc_range(m.getOps<func::FuncOp>())) {
    if (funcOp.isDeclaration())
      continue;
    Region *funcRegion = funcOp.getCallableRegion();
    for (Operation &op : funcRegion->getOps()) {
      if (!isStreamOp(&op))
        continue;
      for (auto &r : op.getRegions()) {
        StreamLowering sl(r);
        if (failed(lowerRegion<YieldOp>(sl, false, false)))
          return failure();
        if (failed(dematerializeForksAndSinks(r)))
          return failure();
        removeBasicBlocks(r);
      }
    }
  }
  return success();
}

static LogicalResult removeUnusedConversionCasts(ModuleOp m) {
  for (auto funcOp : m.getOps<handshake::FuncOp>()) {
    if (funcOp.isDeclaration())
      continue;
    Region &funcRegion = funcOp.body();
    for (auto op : llvm::make_early_inc_range(
             funcRegion.getOps<UnrealizedConversionCastOp>())) {
      op->erase();
    }
  }
  return success();
}

class StreamToHandshakePass
    : public StreamToHandshakeBase<StreamToHandshakePass> {
public:
  void runOnOperation() override {
    if (failed(transformStdRegions(getOperation()))) {
      signalPassFailure();
      return;
    }

    StreamTypeConverter typeConverter;
    RewritePatternSet patterns(&getContext());
    ConversionTarget target(getContext());
    SymbolUniquer symbolUniquer(getOperation());

    // Patterns to lower stream dialect operations
    populateStreamToHandshakePatterns(typeConverter, symbolUniquer, patterns);
    target.addLegalOp<ModuleOp>();
    target.addLegalOp<UnrealizedConversionCastOp>();
    target.addLegalDialect<handshake::HandshakeDialect>();
    target.addLegalDialect<arith::ArithmeticDialect>();
    target.addIllegalDialect<func::FuncDialect>();
    target.addIllegalDialect<StreamDialect>();

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns)))) {
      signalPassFailure();
      return;
    }

    if (failed(removeUnusedConversionCasts(getOperation()))) {
      signalPassFailure();
      return;
    }

    if (failed(materializeForksAndSinks(getOperation())))
      signalPassFailure();
  }
};
} // namespace

std::unique_ptr<Pass> circt_stream::createStreamToHandshakePass() {
  return std::make_unique<StreamToHandshakePass>();
}

