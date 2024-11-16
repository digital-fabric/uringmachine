require_relative "helper"

require "uringmachine/micro_ssl"

class TestSSL < Minitest::Test

  if false
    def test_raises_with_invalid_keystore_file
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.keystore = "/no/such/keystore" }
      assert_equal("Keystore file '/no/such/keystore' does not exist", exception.message)
    end

    def test_raises_with_unreadable_keystore_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.keystore = "/unreadable/keystore" }
          assert_equal("Keystore file '/unreadable/keystore' is not readable", exception.message)
        end
      end
    end
  else
    def test_raises_with_invalid_key_file
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.key = "/no/such/key" }
      assert_equal("Key file '/no/such/key' does not exist", exception.message)
    end

    def test_raises_with_unreadable_key_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.key = "/unreadable/key" }
          assert_equal("Key file '/unreadable/key' is not readable", exception.message)
        end
      end
    end

    def test_raises_with_invalid_cert_file
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.cert = "/no/such/cert" }
      assert_equal("Cert file '/no/such/cert' does not exist", exception.message)
    end

    def test_raises_with_unreadable_cert_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.key = "/unreadable/cert" }
          assert_equal("Key file '/unreadable/cert' is not readable", exception.message)
        end
      end
    end

    def test_raises_with_invalid_key_pem
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.key_pem = nil }
      assert_equal("'key_pem' is not a String", exception.message)
    end

    def test_raises_with_unreadable_ca_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.ca = "/unreadable/cert" }
          assert_equal("ca file '/unreadable/cert' is not readable", exception.message)
        end
      end
    end

    def test_raises_with_invalid_cert_pem
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.cert_pem = nil }
      assert_equal("'cert_pem' is not a String", exception.message)
    end

    def test_raises_with_invalid_key_password_command
      ctx = UM::SSL::Context.new
      ctx.key_password_command = '/unreadable/decrypt_command'

      assert_raises(Errno::ENOENT) { ctx.key_password }
    end
  end
end
