# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'
require 'thread'

COUNT = 10
CONCURRENCY = (ENV['CONCURRENCY'] || 4).to_i

p(CONCURRENCY: CONCURRENCY)

STRING = "foo...bar"
REGEXP = /foo(.+)bar/

def threads_setup
  @threads_mutex = Mutex.new
  @threads_start = Queue.new
  @threads_done = Queue.new
  @threads = CONCURRENCY.times.map {
    Thread.new do
      loop do
        @threads_start.shift
        COUNT.times do
          @threads_mutex.synchronize do
            COUNT.times do
              STRING.match(REGEXP)[1]
            end
          end
        end
        @threads_done << true
      end
    end
  }
end

def threads_run
  @threads_start << true
  @threads_done.shift
end

def um_setup
  @um_machine = UM.new
  @um_mutex = UM::Mutex.new
  @um_start = UM::Queue.new
  @um_done = UM::Queue.new
  @fibers = CONCURRENCY.times.map {
    @um_machine.spin do
      loop do
        @um_machine.shift(@um_start)
        COUNT.times do
          @um_machine.synchronize(@um_mutex) do
            COUNT.times do
              STRING.match(REGEXP)[1]
            end
          end
        end
        @um_machine.push(@um_done, true)
      end
    end
  }
end

def um_run
  @um_machine.push(@um_start, true)
  @um_machine.shift(@um_done)
end

threads_setup
um_setup

Benchmark.ips do |x|
  x.report("threads") { @threads_run }
  x.report("UM")      { @um_run }

  x.compare!(order: :baseline)
end
