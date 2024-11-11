# frozen_string_literal: true

require_relative '../lib/uringmachine'

ITERATIONS = 1000
DEV_NULL = File.open('/dev/null', 'w')
FD = DEV_NULL.fileno
BUF = ('*' * 8192).freeze

$machine = UringMachine.new

def run_um_write
  $machine.write(FD, BUF)
end

1000.times { run_um_write }
