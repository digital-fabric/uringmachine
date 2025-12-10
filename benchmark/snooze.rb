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

$machine = UringMachine.new

def run_snooze_um
  count = 0

  f1 = $machine.spin {
    ITERATIONS.times { count += 1; $machine.snooze }
  }
  f2 = $machine.spin {
    ITERATIONS.times { count += 1; $machine.snooze }
  }

  $machine.join(f1, f2)

  count
end

def run_raw_transfer
  count = 0
  main = Fiber.current
  f2 = nil
  f1 = Fiber.new do
    ITERATIONS.times do
      count += 1
      f2.transfer(nil)
    end
  end

  f2 = Fiber.new do
    ITERATIONS.times do
      count += 1
      f1.transfer(nil)
    end
    main.transfer(nil)
  end

  f1.transfer(nil)

  count
end

p run_snooze_um:    run_snooze_um
p run_raw_transfer: run_raw_transfer

bm = Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("snooze_um")     { run_snooze_um }
  x.report("raw transfer")  { run_raw_transfer }

  x.compare!
end
