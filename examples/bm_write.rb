# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
end

require 'benchmark'
require 'uringmachine'

ITERATIONS = 100000
BUF = ('*' * 8192).freeze
FN = '/tmp/bm_write'

def run_io_write(num_threads)
  FileUtils.rm(FN) rescue nil
  fio = File.open(FN, 'w')

  threads = num_threads.times.map do |i|
    Thread.new do
      ITERATIONS.times { fio.write(BUF) }
    end
  end
  threads.each(&:join)
ensure
  fio.close
end

def run_um_write(num_fibers)
  FileUtils.rm(FN) rescue nil
  fio = File.open(FN, 'w')
  fd = fio.fileno

  machine = UringMachine.new
  done = UringMachine::Queue.new
  num_fibers.times do
    machine.spin do
      ITERATIONS.times { machine.write(fd, BUF) }
      machine.push(done, true)
    end
  end
  num_fibers.times { machine.pop(done) }
ensure
  fio.close
end

Benchmark.bm do |x|
  [1, 2, 4, 8].each do |c|
    x.report("IO (#{c} threads)")     { run_io_write(c) }
    x.report("UM (#{c} fibers) ")     { run_um_write(c) }
    puts
  end
end
