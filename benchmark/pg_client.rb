require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
  gem 'pg'
end

require 'uringmachine/fiber_scheduler'
require 'securerandom'

CONTAINER_NAME = "pg-#{SecureRandom.hex}"

def start_pg_server
  `docker run --name #{CONTAINER_NAME} -e POSTGRES_PASSWORD=my_password -d -p 5432:5432 postgres`
end

def stop_pg_server
  `docker stop #{CONTAINER_NAME}`
end

PG_OPTS = {
  host:                         'localhost',
  user:                         'postgres',
  password:                     'my_password',
  dbname:                       'postgres'
}

def create_db_conn(retries = 0)
  PG.connect(PG_OPTS)
rescue PG::ConnectionBad
  if retries < 3
    sleep 0.5
    create_db_conn(retries + 1)
  else
    raise
  end
end

PREPARE_SQL = <<~SQL
  create table if not exists foo(value int, name text);
  create index if not exists idx_foo_value on foo(value);
  with t1 as (
    select generate_series(1,100000)
  ),
  t2 as (
    select (random()*1000000)::integer as value,
          (md5(random()::text)) as name
    from t1
  )
  insert into foo(value, name) select * from t2;
SQL

def prepare_db
  STDOUT << "Preparing database..."
  t0 = Time.now
  conn = create_db_conn
  conn.exec(PREPARE_SQL)
  puts " elapsed: #{Time.now - t0}"
end

def query_db(conn)
  min = rand(1000000)
  max = min + 2000
  sql = format(
    'select * from foo where value >= %d and value < %d;', min, max
  )
  conn.query(sql).to_a
end

def with_container
  start_pg_server
  sleep 0.5
  prepare_db
  yield
rescue Exception => e
  p e
  p e.backtrace
ensure
  stop_pg_server
end

C = ENV['C']&.to_i ||10
I = 1000
puts "C=#{C}"

def run_threads
  threads = C.times.map do
    Thread.new do
      conn = create_db_conn
      I.times { query_db(conn) }
    ensure
      conn.close
    end
  end
  threads.each(&:join)
end

def run_async_fiber_scheduler
  scheduler = Async::Scheduler.new
  Fiber.set_scheduler(scheduler)
  scheduler.run do
    C.times do
      Fiber.schedule do
        conn = create_db_conn
        I.times { query_db(conn) }
      ensure
        conn.close
      end
    end
  end
end

def run_um_fiber_scheduler
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
  Fiber.set_scheduler(scheduler)

  C.times do
    Fiber.schedule do
      conn = create_db_conn
      I.times { query_db(conn) }
    ensure
      conn.close
    end
  end
  scheduler.join
end

with_container do
  Benchmark.bm do |x|
    x.report("Threads")   { run_threads }
    x.report("Async FS")  { run_async_fiber_scheduler }
    x.report("UM FS")     { run_um_fiber_scheduler }
  end
end
