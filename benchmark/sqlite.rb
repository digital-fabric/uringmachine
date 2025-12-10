# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'extralite'
  gem 'benchmark'
  gem 'benchmark-ips'
end

require 'uringmachine'
require 'extralite'
require 'benchmark/ips'
require 'securerandom'
require 'uringmachine/actor'

class DBActor2
  def initialize(machine, db)
    @machine = machine
    @db = db
    @mailbox = UM::Queue.new
  end

  def act
    while (a, k, peer = @machine.shift(@mailbox))

      begin
        ret = @db.query(*a, **k)
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

  def query(*a, **k)
    @machine.push(@mailbox, [a, k, Fiber.current])
    ret = @machine.yield
    raise(ret) if ret.is_a?(Exception)
    ret
  end

  def self.spin(machine, db)
    actor = new(machine, db)
    machine.spin { actor.act }
    actor
  end
end

PATH = "/tmp/um_bm_sqlite_#{SecureRandom.hex}"

module DBActor
  def setup(db)
    @db = db
  end

  def query(*, **)
    @db.query(*, **)
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

def prepare_db(machine, path)
  Extralite::Database.new(path).tap {
    it.on_progress(mode: :at_least_once, period: 100, tick: 10) { machine.snooze }
  }
end

machine = UM.new
mailbox = UM::Queue.new
raw_db = prepare_db(machine, PATH)
actor_db = machine.spin_actor(DBActor, prepare_db(machine, PATH))
actor2_db = DBActor2.spin(machine, prepare_db(machine, PATH))
locker_db = Locker.new(machine, prepare_db(machine, PATH))

p raw_db.query('select 1')
p actor_db.call(mailbox, :query, 'select 1')
p actor2_db.query('select 1')
p locker_db.query('select 1')

bm = Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("raw")     { raw_db.query('select 1') }
  x.report("actor")   { actor_db.call(mailbox, :query, 'select 1') }
  x.report("actor2")  { actor2_db.query('select 1') }
  x.report("locker")  { locker_db.query('select 1') }

  x.compare!(order: :baseline)
end
