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

GROUPS = 16

SIZE = 1 << 16
DATA = '*' * SIZE

def threads_setup
  @threads_start_queue = Queue.new
  @threads_stop_queue = Queue.new

  GROUPS.times do
    r, w = IO.pipe
    r.sync = true
    w.sync = true
    Thread.new do
      loop do
        iterations = @threads_start_queue.shift
        iterations.times { w.write(DATA) }
        @threads_stop_queue << true
      end
    end
    Thread.new do
      loop do
        iterations = @threads_start_queue.shift
        iterations.times { r.read(SIZE) }
        @threads_stop_queue << true
      end
    end
  end
end

def threads_run(times)
  (GROUPS * 2).times { @threads_start_queue << times }
  (GROUPS * 2).times { @threads_stop_queue.shift }
end

def um_setup
  @machine = UM.new
  
  @um_start_queue = UM::Queue.new
  @um_stop_queue = UM::Queue.new

  GROUPS.times do
    r, w = UM.pipe
    @machine.spin do
      loop do
        iterations = @machine.shift(@um_start_queue)
        iterations.times { @machine.writev(w, DATA) }
        @machine.push(@um_stop_queue, true)
      end
    end
    @machine.spin do
      loop do
        iterations = @machine.shift(@um_start_queue)
        iterations.times {
          left = SIZE
          left -= @machine.read(r, +'', left) while left > 0
        }
        @machine.push(@um_stop_queue, true)
      end
    end
  end
end

def um_run(times)
  (GROUPS * 2).times { @machine.push(@um_start_queue, times) }
  (GROUPS * 2).times { @machine.shift(@um_stop_queue) }
end

def um2_setup
  @um2_start_queue = UM::Queue.new
  @um2_stop_queue = UM::Queue.new

  thread_count = 2
  tgroups = GROUPS / thread_count
  
  @um2_machines = []
  @um2_done = UM::Queue.new
  @um2_threads = thread_count.times.map do
    Thread.new do
      machine = UM.new
      @um2_machines << machine
      tgroups.times do
        r, w = UM.pipe
        machine.spin do
          loop do
            iterations = machine.shift(@um2_start_queue)
            iterations.times { machine.writev(w, DATA) }
            machine.push(@um2_stop_queue, true)
          end
        end
        machine.spin do
          loop do
            iterations = machine.shift(@um2_start_queue)
            iterations.times {
              left = SIZE
              left -= machine.read(r, +'', left) while left > 0
            }
            machine.push(@um2_stop_queue, true)
          end
        end
      end
      machine.shift(@um2_done)
    end
  end
end

def um2_teardown
  2.times { @machine.push(@um2_done, true) }
end

def um2_run(times)
  (GROUPS * 2).times { @machine.push(@um2_start_queue, times) }
  (GROUPS * 2).times { @machine.shift(@um2_stop_queue); @machine.snooze }
end

threads_setup
um_setup
um2_setup

at_exit { um2_teardown }

# um2_run(1)
# p after_run: 1

Benchmark.ips do |x|
  x.report('Threads') { |t| threads_run(t) }
  x.report('UM')      { |t| um_run(t) }
  x.report('UMx2')    { |t| um2_run(t) }

  x.compare!(order: :baseline)
end
