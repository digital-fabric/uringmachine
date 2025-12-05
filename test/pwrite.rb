# frozen_string_literal: true

require 'bundler/setup'
require 'uringmachine'
require 'uringmachine/fiber_scheduler'
require 'securerandom'

class MethodCallAuditor
  attr_reader :calls
  
  def initialize(target)
    @target = target
    @calls = []
  end

  def respond_to?(sym, include_all = false) = @target.respond_to?(sym, include_all)
  
  def method_missing(sym, *args, &block)
    res = @target.send(sym, *args, &block)
    @calls << ({ sym:, args:, res:})
    res
  rescue => e
    @calls << ({ sym:, args:, res: e})
    raise
  end

  def last_call
    calls.last
  end
end

@machine = UM.new
@raw_scheduler = UM::FiberScheduler.new(@machine)
@scheduler = MethodCallAuditor.new(@raw_scheduler)
Fiber.set_scheduler(@scheduler)


p pid: Process.pid
fn = "/tmp/#{SecureRandom.hex}"
IO.write(fn, 'foobar')

res = nil
Fiber.schedule do
  File.open(fn, 'r+') do |f|
    # res = f.write('baz')
    res = f.pwrite('baz', 2)
  end
end

@scheduler.join
p(res:)

puts "file content: #{IO.read(fn)}"

p @scheduler.calls.map { it[:sym] }.tally
