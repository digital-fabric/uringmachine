# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark-ips'
  gem 'http_parser.rb'
end

require 'benchmark/ips'
require 'uringmachine'
require 'http/parser'

$machine = UM.new

HTTP_MSG = "GET /foo/bar HTTP/1.1\r\nServer: foobar.com\r\nFoo: bar\r\n\r\n"

$count = 0

def parse_http_parser
  current_fiber = Fiber.current
  $count += 1
  r, w = IO.pipe
  parser = Http::Parser.new
  $machine.spin do
    buffer = +''
    loop do
      res = $machine.read(r.fileno, buffer, 512)
      break if res == 0
      parser << buffer
    end
  rescue Exception => e
    $machine.schedule(current_fiber, e)
    # puts e.backtrace.join("\n")
    # exit!
  end
  parser.on_message_complete = -> do
    headers = parser.headers
    headers['method'] = parser.http_method.downcase
    headers['path'] = parser.request_url
    headers['protocol'] = parser.http_version
    $machine.schedule(current_fiber, headers)
  end
  
  $machine.write(w.fileno, HTTP_MSG)
  $machine.yield
ensure
  $machine.close(r.fileno)
  $machine.close(w.fileno)
end

require 'stringio'

RE_REQUEST_LINE = /^([a-z]+)\s+([^\s]+)\s+(http\/[0-9\.]{1,3})/i
RE_HEADER_LINE = /^([a-z0-9\-]+)\:\s+(.+)/i

def get_line(fd, sio, buffer)
  while true
    line = sio.gets(chomp: true)
    return line if line

    res = $machine.read(fd, buffer, 1024, -1)
    return nil if res == 0
  end
end

def get_request_line(fd, sio, buffer)
  line = get_line(fd, sio, buffer)

  m = line.match(RE_REQUEST_LINE)
  return nil if !m

  {
    'method'   => m[1].downcase,
    'path'     => m[2],
    'protocol' => m[3].downcase
  }
end

def parse_headers(fd)
  buffer = String.new('', capacity: 4096)
  sio = StringIO.new(buffer)

  headers = get_request_line(fd, sio, buffer)
  return nil if !headers

  while true
    line = get_line(fd, sio, buffer)
    break if line.empty?
    
    m = line.match(RE_HEADER_LINE)
    raise "Invalid header" if !m

    headers[m[1]] = m[2]
  end

  headers
end

def parse_http_stringio
  current_fiber = Fiber.current
  r, w = IO.pipe

  $machine.spin do
    headers = parse_headers(r.fileno)
    $machine.schedule(current_fiber, headers)
  rescue Exception => e
    p e
    puts e.backtrace.join("\n")
    exit!
  end
  
  $machine.write(w.fileno, HTTP_MSG)
  $machine.yield
ensure
  $machine.close(r.fileno)
  $machine.close(w.fileno)
end

# p parse_http_parser
# p parse_http_stringio
# exit

GC.disable

def alloc_count
  count0 = ObjectSpace.count_objects[:TOTAL]
  yield
  count1 = ObjectSpace.count_objects[:TOTAL]
  count1 - count0
end

X = 100
p(
  alloc_http_parser: alloc_count { X.times { parse_http_parser } },
  alloc_stringio: alloc_count { X.times { parse_http_stringio } }
)
exit

Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 3)

  x.report("http_parser") { parse_http_parser }
  x.report("homegrown") { parse_http_stringio }

  x.compare!
end
