# frozen_string_literal: true

require_relative './common'
require 'securerandom'

C = ENV['C']&.to_i || 50
I = 100
puts "C=#{C}"

class UMBenchmark
  CONTAINER_NAME = "redis-#{SecureRandom.hex}"

  def start_redis_server
    `docker run --name #{CONTAINER_NAME} -d -p 6379:6379 redis:latest`
    create_redis_conn
  end

  def stop_redis_server
    `docker stop #{CONTAINER_NAME}`
  end

  def create_redis_conn(retries = 0)
    Redis.new
  rescue
    if retries < 3
      sleep 0.2
      create_redis_conn(retries + 1)
    else
    raise
    end
  end

  def query_redis(conn)
    conn.set('abc', 'def')
    conn.get('abc')
  end

  def with_container
    start_redis_server
    sleep 0.5
    yield
  rescue Exception => e
    p e
    p e.backtrace
  ensure
  stop_redis_server
  end

  def create_redis_conn(retries = 0)
    Redis.new
  rescue
    raise if retries >= 3

    sleep 0.5
    create_redis_conn(retries + 1)
  end

  def benchmark
    with_container {
      Benchmark.bm { run_benchmarks(it) }
    }
  end

  def do_threads(threads, ios)
    C.times.map do
      threads << Thread.new do
        conn = create_redis_conn
        I.times { query_redis(conn) }
      ensure
        conn.close
      end
    end
  end

  def do_scheduler(scheduler, ios)
    C.times do
      Fiber.schedule do
        conn = create_redis_conn
        I.times { query_redis(conn) }
      ensure
        conn.close
      end
    end
  end
end
