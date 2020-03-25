defmodule MosquittoEmbed.Driver do
    use GenServer
    require Logger

    # These must be kept in sync with mosquitto_embed.c
    @cmd_echo 0

    @servername __MODULE__
    @portname 'mosquitto_embed'

    def start_link(args \\ []) do
        GenServer.start_link(__MODULE__, args, name: @servername)
    end

    def hello(msg) do
        GenServer.call(@servername, {:hello, msg})
    end

    def init(args) do
        # Make sure the driver is loaded 
        # (ignore any error if it already is)
        port_path = :code.priv_dir(:mosquitto_embed)
        case :erl_ddll.load_driver(port_path, @portname) do
            :ok -> :ok;
            {:error, :already_loaded} -> :ok;
            {:error, error_desc} -> 
                Logger.error("Cannot Load #{port_path} #{@portname} #{:erl_ddll.format_error(error_desc)}")
        end

        port = :erlang.open_port({:spawn, @portname}, [:binary])
        state = %{port: port, waiters: []}
        {:ok, state}
    end

    def handle_call({:hello, msg}, from, state = %{port: port, waiters: waiters}) do
        #:erlang.port_command(port, msg)
        Logger.debug("control #{inspect(:erlang.binary_to_term(:erlang.port_control(port, @cmd_echo, msg)))}")
        {:noreply, %{state | waiters: waiters ++ [from] }}
    end

    def handle_info(:stop, state = %{port: port}) do
        :erlang.port_close(port)
        {:noreply, state}
    end

    def handle_info({port,{:data,data}}, state = %{port: port, waiters: [waiter | waiters]}) do
        Logger.debug("Data: #{inspect(data)}")
        GenServer.reply(waiter, data)
        state = handle_data(data, state)
        {:noreply, %{state | waiters: waiters} };
    end

    def handle_data(data, state) do
        state
    end    
end