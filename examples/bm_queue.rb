# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'

COUNT = 1000
NUM_PRODUCERS = 2
NUM_CONSUMERS = 2

@queue = Queue.new
@done = Queue.new

def run_threads

  NUM_PRODUCERS.times do
    Thread.new do
      COUNT.times { @queue << rand(1000) }
      @done << true
    end
  end

  total = 0
  NUM_CONSUMERS.times do
    Thread.new do
      loop do
        item = @queue.shift
        break if item.nil?

        total += item
      end
      @done << true
    end
  end

  # wait for producers
  NUM_PRODUCERS.times { @done.shift }

  # stop and wait for consumers
  NUM_CONSUMERS.times do
    @queue << nil
    @done.shift
  end

  total
end

@machine = UM.new
@um_queue = UM::Queue.new
@um_done = UM::Queue.new

def run_um
  NUM_PRODUCERS.times do
    @machine.spin do
      COUNT.times { @machine.push(@um_queue, rand(1000)) }
      @machine.push(@um_done, true)
    end
  end

  total = 0
  NUM_CONSUMERS.times do
    @machine.spin do
      loop do
        item = @machine.shift(@um_queue)
        break if item.nil?

        total += item
      end
      @machine.push(@um_done, true)
    end
  end

  # wait for producers
  NUM_PRODUCERS.times { @machine.shift(@um_done) }

  # stop and wait for consumers
  NUM_CONSUMERS.times do
    @machine.push(@um_queue, nil)
    @machine.shift(@um_done)
  end

  total
end


# puts "running"
# res = run_threads
# p threads: res

# 100.times {
#   res = run_um
#   p fibers: res
# }


# __END__

Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("threads") { run_threads }
  x.report("UM")      { run_um }

  x.compare!
end
