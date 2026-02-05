# frozen_string_literal: true

require 'um_ext'
require 'uringmachine/version'
require 'uringmachine/dns_resolver'

UM = UringMachine

class UringMachine
  def fiber_map
    @fiber_map ||= {}
  end

  class Terminate < Exception
  end

  def spin(value = nil, klass = Fiber, &block)
    fiber = klass.new { |v| run_block_in_fiber(block, fiber, v) }
    self.schedule(fiber, value)

    fiber_map[fiber] = fiber
  end

  def run(fiber, &block)
    run_block_in_fiber(block, fiber, nil)
    self.schedule(fiber, nil)
    fiber_map[fiber] = fiber
  end

  def join(*fibers)
    results = fibers.inject({}) { |h, f| h[f] = nil; h }
    queue = nil
    pending = nil
    fibers.each do |f|
      if f.done?
        results[f] = f.result
      else
        (pending ||= []) << f
        queue ||= Fiber.current.mailbox
        f.add_done_listener(queue)
      end
    end
    if pending
      while !pending.empty?
        f = self.shift(queue)
        pending.delete(f)
        results[f] = f.result
      end
    end
    values = results.values
    fibers.size == 1 ? values.first : values
  end

  def await_fibers(fibers)
    if fibers.is_a?(Fiber)
      f = fibers
      if !f.done?
        queue = Fiber.current.mailbox
        f.add_done_listener(queue)
        self.shift(queue)
      end
      return 1
    end

    queue = nil
    pending = nil
    fibers.each do |f|
      if !f.done?
        (pending ||= []) << f
        queue ||= Fiber.current.mailbox
        f.add_done_listener(queue)
      end
    end
    if pending
      while !pending.empty?
        f = self.shift(queue)
        pending.delete(f)
      end
    end
    fibers.count
  end

  def resolve(hostname, type = :A)
    @resolver ||= DNSResolver.new(self)
    @resolver.resolve(hostname, type)
  end

  def file_watch(root, mask)
    fd = UM.inotify_init
    wd_map = {}
    recursive_file_watch(fd, root, wd_map, mask)
    while true
      events = inotify_get_events(fd)
      events.each do |event|
        if event[:mask] | UM::IN_IGNORED == UM::IN_IGNORED
          wd_map.delete(event[:wd])
          next
        end
        transformed_event = transform_file_watch_event(fd, event, wd_map, mask)
        if event[:mask] == UM::IN_CREATE | UM::IN_ISDIR
          recursive_file_watch(fd, transformed_event[:fn], wd_map, mask)
        end
        yield transformed_event
      end
    end
  ensure
    close_async(fd)
  end

  private

  def run_block_in_fiber(block, fiber, value)
    ret = block.(value)
    fiber.set_result(ret)
  rescue Exception => e
    fiber.set_result(e)
  ensure
    fiber.mark_as_done
    # cleanup
    fiber_map.delete(fiber)
    self.notify_done_listeners(fiber)

    # switch away to a different fiber
    self.switch
  end

  def notify_done_listeners(fiber)
    listeners = fiber.done_listeners
    return if !listeners

    listeners.each { self.push(it, fiber) }
  end

  def recursive_file_watch(fd, dir, wd_map, mask)
    wd = UM.inotify_add_watch(fd, dir, mask)
    wd_map[wd] = dir
    Dir[File.join(dir, '*')].each do
      recursive_file_watch(fd, it, wd_map, mask) if File.directory?(it)
    end
  end

  def transform_file_watch_event(fd, event, wd_map, mask)
    {
      mask: event[:mask],
      fn: File.join(wd_map[event[:wd]], event[:name])
    }
  end

  module FiberExtensions
    attr_reader :result, :done, :done_listeners

    def mark_as_done
      @done = true
    end

    def set_result(value)
      @result = value
    end

    def done?
      @done
    end

    def add_done_listener(queue)
      (@done_listeners ||= []) << queue
    end

    def mailbox
      @mailbox ||= UM::Queue.new
    end
  end

  class ::Fiber
    include FiberExtensions
  end

  module ThreadExtensions
    def machine
      @machine ||= UM.new
    end
  end

  class ::Thread
    include ThreadExtensions
  end
end
