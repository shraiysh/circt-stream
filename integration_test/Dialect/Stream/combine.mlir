// REQUIRES: verilator
// RUN: stream-opt %s --convert-stream-to-handshake \
// RUN:   --canonicalize='top-down=true region-simplify=true' \
// RUN:   --handshake-materialize-forks-sinks --canonicalize \
// RUN:   --handshake-insert-buffers=strategy=all --lower-handshake-to-firrtl | \
// RUN: firtool --format=mlir --verilog > %t.sv && \
// RUN: circt-rtl-sim.py %t.sv %S/driver_out_i64.sv %S/driver.cpp --no-default-driver --top driver | FileCheck %s
// CHECK: Element={{.*}}11
// CHECK-NEXT: Element={{.*}}13
// CHECK-NEXT: Element={{.*}}15
// CHECK-NEXT: EOS

module {
  func.func @top() -> (!stream.stream<i64>) {
    %in0 = stream.create !stream.stream<i64> [1,2,3]
    %in1 = stream.create !stream.stream<i64> [10,11,12]
    %res = stream.combine(%in0, %in1) : (!stream.stream<i64>, !stream.stream<i64>) -> (!stream.stream<i64>) {
    ^0(%val0: i64, %val1: i64):
    %0 = arith.addi %val0, %val1 : i64
      stream.yield %0 : i64
    }
    return %res : !stream.stream<i64>
  }
}
