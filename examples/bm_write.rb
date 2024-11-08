# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'

ITERATIONS = 1000
DEV_NULL = File.open('/dev/null', 'w')
FD = DEV_NULL.fileno
BUF = ('*' * 8192).freeze

def run_io_write
  DEV_NULL.write(BUF)
end

$machine = UringMachine.new

def run_um_write
  $machine.write(FD, BUF)
end

bm = Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("io_write")  { run_io_write }
  x.report("um_write")  { run_um_write }

  x.compare!
end
