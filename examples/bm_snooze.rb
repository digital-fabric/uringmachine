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

def run_snooze
  count = 0
  main = Fiber.current

  f1 = Fiber.new do
    loop do
      count += 1
      if count == ITERATIONS
        $machine.schedule(main, nil)
        break
      else
        $machine.snooze
      end
    end
  end

  f2 = Fiber.new do
    loop do
      count += 1
      if count == ITERATIONS
        $machine.schedule(main, nil)
        break
      else
        $machine.snooze
      end
    end
  end

  $machine.schedule(f1, nil)
  $machine.schedule(f2, nil)
  $machine.yield
end

def run_raw_transfer
  count = 0
  main = Fiber.current
  f2 = nil
  f1 = Fiber.new do
    loop do
      count += 1
      if count == ITERATIONS
        main.transfer(nil)
        break
      else
        f2.transfer(nil)
      end
    end
  end

  f2 = Fiber.new do
    loop do
      count += 1
      if count == ITERATIONS
        main.transfer(nil)
        break
      else
        f1.transfer(nil)
      end
    end
  end

  f1.transfer(nil)
end

bm = Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("snooze")        { run_snooze }
  x.report("raw transfer")  { run_raw_transfer }

  x.compare!
end
