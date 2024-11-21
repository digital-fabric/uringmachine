# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'extralite'
  gem 'benchmark-ips'
end

require 'uringmachine'
require 'extralite'

class UM::Actor < Fiber
  def initialize(machine, target)
    @machine = machine
    @target = target
    @mailbox = UM::Queue.new
    super { act }
  end

  def act
    while (sym, a, k, peer = @machine.shift(@mailbox))

      begin
        ret = @target.send(sym, *a, **k)
        @machine.schedule(peer, ret)
      rescue => e
        @machine.schedule(peer, e)
      end
    end
  rescue Exception => e
    # handle unhandled exceptions
  ensure
    @machine.fiber_map.delete(self)
    @machine.yield
  end

  def method_missing(sym, *a, **k)
    @machine.push(@mailbox, [sym, a, k, Fiber.current])
    ret = @machine.yield
    raise(ret) if ret.is_a?(Exception)
    ret
  end
end

class UM
  def spin_actor(target)
    f = UM::Actor.new(self, target)
    schedule(f, nil)
    @@fiber_map[f] = true
    f
  end
end

class Locker
  def initialize(machine, target)
    @machine = machine
    @target = target
    @mutex = UM::Mutex.new
  end

  def method_missing(sym, *a, **k)
    @machine.synchronize(@mutex) { @target.send(sym, *a, **k) }
  end
end


PATH = '/tmp/foo'

$machine = UM.new
$raw_db = Extralite::Database.new(PATH)
$actor_db = $machine.spin_actor(Extralite::Database.new(PATH))
$locker_db = Locker.new($machine, Extralite::Database.new(PATH))

[$raw_db, $actor_db, $locker_db].each do |db|
  p db.query('select 1')
end

bm = Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("raw")     { $raw_db.query('select 1') }
  x.report("actor")   { $actor_db.query('select 1') }
  x.report("locker")  { $locker_db.query('select 1') }

  x.compare!
end
