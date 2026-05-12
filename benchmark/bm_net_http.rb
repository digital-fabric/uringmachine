# frozen_string_literal: true

require_relative './common'
require_relative './bm_net_http_support'

require 'net/http'

CONCURRENCY = ENV['C']&.to_i || 50
ITERATIONS = ENV['I']&.to_i || 200

# Adapted from benchmark code in https://github.com/yaroslav/carbon_fiber

RESPONSE_BODY = %({"ok":true,"value":12345})
REQUEST_PATH = "/api"

SERVER = LoopbackServer.new(
  response_body: RESPONSE_BODY,
  content_type: "application/json"
)
PORT = SERVER.port
sleep(0.1)

class UMBenchmark
  def run_http_client
    Net::HTTP.start("127.0.0.1", PORT, nil, nil) do |http|
      ITERATIONS.times do
        request = Net::HTTP::Get.new(REQUEST_PATH)
        response = http.request(request)
        raise "unexpected status #{response.code}" unless response.code == "200"
        raise "unexpected body size" unless response.body&.bytesize == RESPONSE_BODY.bytesize
      end
    end
  rescue => e
    p e
    p e.backtrace
    exit!
  end

  def do_threads(threads, ios)
    CONCURRENCY.times do
      threads << Thread.new { run_http_client }
    end
  end

  def do_scheduler(scheduler, ios)
    CONCURRENCY.times do
      Fiber.schedule { run_http_client }
    end
  end

  def do_scheduler_x(div, scheduler, ios)
    (CONCURRENCY/div).times { run_http_client }
  end
end
