# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
  gem 'pg'
  gem 'gvltools'
  gem 'openssl'
  gem 'localhost'
end

require 'uringmachine/fiber_scheduler'
require 'socket'
require 'openssl'
require 'localhost/authority'

GROUPS = 96
ITERATIONS = 10000

SIZE = 1 << 14
DATA = '*' * SIZE

AUTHORITY = Localhost::Authority.fetch

def do_io(um)
  GROUPS.times do
    r, w = Socket.socketpair(:AF_UNIX, :SOCK_STREAM, 0)
    r.sync = true
    w.sync = true
    ctx = AUTHORITY.server_context
    ctx.verify_mode = OpenSSL::SSL::VERIFY_NONE
    s1 = OpenSSL::SSL::SSLSocket.new(r, ctx)
    s1.sync_close = true
    um.ssl_set_bio(s1) if um
    Fiber.schedule do
      s1.accept
      ITERATIONS.times { s1.readpartial(SIZE) }
      s1.close rescue nil
    rescue => e
      p e
      p e.backtrace
    end

    s2 = OpenSSL::SSL::SSLSocket.new(w, OpenSSL::SSL::SSLContext.new)
    s2.sync_close = true
    um.ssl_set_bio(s2) if um
    Fiber.schedule do
      s2.connect
      ITERATIONS.times { s2.write(DATA) }
      s2.close rescue nil
    rescue => e
      p e
      p e.backtrace
    end
  end
end

def run(custom_bio)
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
  Fiber.set_scheduler(scheduler)
  do_io(custom_bio ? machine : nil)
  scheduler.join
end

Benchmark.bm do |b|
  b.report("stock") { run(false) }
  b.report("UM BIO") { run(true) }
end
