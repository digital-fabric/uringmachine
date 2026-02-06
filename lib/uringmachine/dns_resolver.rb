# frozen_string_literal: true

require 'resolv'

class UringMachine
  # A basic DNS resolver implementation.
  class DNSResolver

    # Initializes the DNS resolver.
    #
    # @param machine [UringMachine] UringMachine instance
    # @return [void]
    def initialize(machine)
      @machine = machine
      @requests = UM::Queue.new
      @nameservers = get_nameservers
      @fiber = @machine.spin { handle_requests_loop }
      @last_id = 0
    end

    # Resolves the given hostname.
    #
    # @param hostname [String] hostname
    # @param type [Symbol] DNS record type
    # @return [String] IP address
    def resolve(hostname, type)
      @machine.push(@requests, [hostname, type, Fiber.current])
      @machine.yield
    end

    private
    
    # Handles resolve requests as they come.
    def handle_requests_loop
      while true
        hostname, type, fiber = @machine.shift(@requests)
        res = do_resolve(hostname, type)
        @machine.schedule(fiber, res)
      end
    end

    # Returns an array of nameservers.
    #
    # @return [Array<String>] name servers
    def get_nameservers
      nameservers = []
      IO.readlines('/etc/resolv.conf').each do |line|
        if line =~ /^nameserver (.+)$/
          nameservers << $1.split(/\s+/).first
        end
      end
      nameservers
    end

    # Returns a DNS socket fd connected to a name server.
    #
    # @return [Integer] fd
    def socket_fd
      @socket_fd ||= prepare_socket
    end

    # Prepares a socket fd connected to a name server.
    #
    # @return [Integer] fd
    def prepare_socket
      fd = @machine.socket(UM::AF_INET, UM::SOCK_DGRAM, 0, 0)
      @machine.bind(fd, '0.0.0.0', 0)
      @machine.connect(fd, @nameservers.sample, 53)
      fd
    end

    # Resolves a DNS query.
    #
    # @param hostname [String] hostname
    # @param type [Symbol] DNS record type
    # @param try_count [Integer] retry counter
    # @return [Array<String>] array of addresses
    def do_resolve(hostname, type, try_count = 0)
      fd = socket_fd
      req = prepare_request_packet(hostname, type)
      msg = req.encode
      @machine.send(fd, msg, msg.bytesize, 0)

      buf = +''
      @machine.recv(fd, buf, 16384, 0)

      msg = Resolv::DNS::Message.decode buf
      addrs = []
      msg.each_answer do |name, ttl, data|
        # p [name, ttl, data]
        if data.kind_of?(Resolv::DNS::Resource::IN::A) ||
          data.kind_of?(Resolv::DNS::Resource::IN::AAAA)
          addrs << data.address.to_s
        end
      end
      addrs
    end

    # Prepares a request packet.
    #
    # @param hostname [String] hostname
    # @param type [Symbol] DNS record type
    # @return [Resolv::DNS::Message]
    def prepare_request_packet(hostname, type)
      msg = Resolv::DNS::Message.new
      msg.id = (@last_id += 1)
      msg.rd = 1
      msg.add_question hostname, msg_type(type)
      msg
    end

    # Returns the message type class.
    #
    # @param type [Symbol] DNS record type
    # @return [Class] message type class
    def msg_type(type)
      # TODO: add support for other types
      Resolv::DNS::Resource::IN::A
    end
  end
end
