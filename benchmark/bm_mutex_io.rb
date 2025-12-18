# frozen_string_literal: true

require_relative './common'
require 'securerandom'
require 'fileutils'

GROUPS = ENV['N']&.to_i || 48
WORKERS = 10
ITERATIONS = 10000

puts "N=#{GROUPS}"

SIZE = 1024
DATA = "*" * SIZE

class UMBenchmark
  def cleanup
    # `rm /tmp/mutex*` rescue nil
  end

  def do_threads(threads, ios)
    GROUPS.times do
      mutex = Mutex.new
      # ios << (f = File.open("/tmp/mutex_io_threads_#{SecureRandom.hex}", 'w'))
      ios << (f = File.open("/dev/null", 'w'))
      f.sync = true
      WORKERS.times do
        threads << Thread.new do
          ITERATIONS.times do
            mutex.synchronize do
              f.write(DATA)
            end
          end
        end
      end
    end
  end

  def do_scheduler(scheduler, ios)
    GROUPS.times do
      mutex = Mutex.new
      # ios << (f = File.open("/tmp/mutex_io_fiber_scheduler_#{SecureRandom.hex}", 'w'))
      ios << (f = File.open("/dev/null", 'w'))
      f.sync = true
      WORKERS.times do
        Fiber.schedule do
          ITERATIONS.times do
            mutex.synchronize { f.write(DATA) }
          end
        end
      end
    end
  end

  def do_scheduler_x(div, scheduler, ios)
    (GROUPS/div).times do
      mutex = Mutex.new
      # ios << (f = File.open("/tmp/mutex_io_fiber_scheduler_#{SecureRandom.hex}", 'w'))
      ios << (f = File.open("/dev/null", 'w'))
      f.sync = true
      WORKERS.times do
        Fiber.schedule do
          ITERATIONS.times do
            mutex.synchronize { f.write(DATA) }
          end
        end
      end
    end
  end

  def do_um(machine, fibers, fds)
    GROUPS.times do
      mutex = UM::Mutex.new
      # fds << (fd = machine.open("/tmp/mutex_io_um_#{SecureRandom.hex}", UM::O_CREAT | UM::O_WRONLY))
      fds << (fd = machine.open("/dev/null", UM::O_WRONLY))
      WORKERS.times do
        fibers << machine.spin do
          ITERATIONS.times do
            machine.synchronize(mutex) do
              machine.write(fd, DATA)
            end
          end
        rescue => e
          p e
        end
      end
    end
  end

  def do_um_x(div, machine, fibers, fds)
    (GROUPS/div).times do
      mutex = UM::Mutex.new
      # fds << (fd = machine.open("/tmp/mutex_io_um_#{SecureRandom.hex}", UM::O_CREAT | UM::O_WRONLY))
      fds << (fd = machine.open("/dev/null", UM::O_WRONLY))
      WORKERS.times do
        fibers << machine.spin do
          ITERATIONS.times do
            machine.synchronize(mutex) do
              machine.write(fd, DATA)
            end
          end
        end
      end
    end
  end
end
