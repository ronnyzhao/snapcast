//
// blocking_tcp_echo_server.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <cstdlib>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <vector>
#include <ctime>   // localtime
#include <sstream> // stringstream
#include <iomanip>
#include <thread>
#include <memory>
#include <set>
#include "common/chunk.h"
#include "common/timeUtils.h"
#include "common/queue.h"
#include "common/signalHandler.h"
#include "common/utils.h"
#include <syslog.h>


using boost::asio::ip::tcp;
namespace po = boost::program_options;

const int max_length = 1024;

typedef boost::shared_ptr<tcp::socket> socket_ptr;
using namespace std;
using namespace std::chrono;


bool g_terminated = false;


std::string return_current_time_and_date()
{
    auto now = system_clock::now();
    auto in_time_t = system_clock::to_time_t(now);
	system_clock::duration ms = now.time_since_epoch();
	char buff[20];
	strftime(buff, 20, "%Y-%m-%d %H:%M:%S", localtime(&in_time_t));
	stringstream ss;
	ss << buff << "." << std::setw(3) << std::setfill('0') << ((ms / milliseconds(1)) % 1000);
    return ss.str();
}


class Session
{
public:
	Session(socket_ptr sock) : active_(false), socket_(sock)
	{
	}

	void sender()
	{
		try
		{
			for (;;)
			{
				shared_ptr<Chunk> chunk(chunks.pop());
				size_t written = 0;
				size_t toWrite = Chunk::getHeaderSize();// + chunk->length;
				WireChunk* wireChunk = chunk->wireChunk;				
				do
				{
					written += boost::asio::write(*socket_, boost::asio::buffer(wireChunk + written, toWrite - written));//, error);
				}
				while (written < toWrite);

				written = 0;
				toWrite = wireChunk->length;
				do
				{
					written += boost::asio::write(*socket_, boost::asio::buffer(wireChunk->payload + written, toWrite - written));//, error);
				}
				while (written < toWrite);
			}
		}
		catch (std::exception& e)
		{
			std::cerr << "Exception in thread: " << e.what() << "\n";
			active_ = false;
		}
	}

	void start()
	{
		active_ = true;
		senderThread = new thread(&Session::sender, this);
//		readerThread.join();
	}

	void send(shared_ptr<Chunk> chunk)
	{
		while (chunks.size() * chunk->getDuration() > 10000)
			chunks.pop();
		chunks.push(chunk);
	}

	bool isActive() const
	{
		return active_;
	}

private:
	bool active_;
	socket_ptr socket_;
	thread* senderThread;
	Queue<shared_ptr<Chunk>> chunks;
};


class Server
{
public:
	Server(unsigned short port) : port_(port)
	{
	}

	void acceptor()
	{
		tcp::acceptor a(io_service_, tcp::endpoint(tcp::v4(), port_));
		for (;;)
		{
			socket_ptr sock(new tcp::socket(io_service_));
			a.accept(*sock);
			cout << "New connection: " << sock->remote_endpoint().address().to_string() << "\n";
			Session* session = new Session(sock);
			session->start();
			sessions.insert(shared_ptr<Session>(session));
		}
	}

	void send(shared_ptr<Chunk> chunk)
	{
		for (std::set<shared_ptr<Session>>::iterator it = sessions.begin(); it != sessions.end(); ) 
		{
    		if (!(*it)->isActive())
			{
				cout << "Session inactive. Removing\n";
		        sessions.erase(it++);
			}
		    else
		        ++it;
	    }

		for (auto s : sessions)
			s->send(chunk);
	}

	void start()
	{
		acceptThread = new thread(&Server::acceptor, this);
	}

	void stop()
	{
//		acceptThread->join();
	}

private:
	set<shared_ptr<Session>> sessions;
	boost::asio::io_service io_service_;
	unsigned short port_;
	thread* acceptThread;
};


class ServerException : public std::exception
{
public:
	ServerException(const std::string& what) : what_(what)
	{
	}

	virtual ~ServerException() throw()
	{
	}

	virtual const char* what() const throw()
	{
		return what_.c_str();
	}

private:
	std::string what_;
};


int main(int argc, char* argv[])
{
	try
	{
		uint16_t sampleRate;
		short channels;
		uint16_t bps;

        size_t port;
        string fifoName;
		bool runAsDaemon;

        po::options_description desc("Allowed options");
        desc.add_options()
            ("help,h", "produce help message")
            ("port,p", po::value<size_t>(&port)->default_value(98765), "port to listen on")
	        ("channels,c", po::value<short>(&channels)->default_value(2), "number of channels")
	       	("samplerate,r", po::value<uint16_t>(&sampleRate)->default_value(48000), "sample rate")
    	    ("bps,b", po::value<uint16_t>(&bps)->default_value(16), "bit per sample")
            ("fifo,f", po::value<string>(&fifoName)->default_value("/tmp/snapfifo"), "name of fifo file")
            ("daemon,d", po::bool_switch(&runAsDaemon)->default_value(false), "daemonize")
        ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help"))
        {
            cout << desc << "\n";
            return 1;
        }

/*        if (vm.count("port") == 0)
        {
            cout << "Please specify server port\n";
            return 1;
        }

//        cout << "Compression level was set to " << vm["compression"].as<int>() << ".\n";

		if (argc == 1)
		{
			std::cerr << desc << "\n";
			return 1;
		}
*/

		if (runAsDaemon)
		{
			daemonize();
			syslog (LOG_NOTICE, "First daemon started.");
		}

		openlog ("firstdaemon", LOG_PID, LOG_DAEMON);

		using namespace std; // For atoi.
		Server* server = new Server(port);
		server->start();

		timeval tvChunk;
		gettimeofday(&tvChunk, NULL);
		long nextTick = getTickCount();

        mkfifo(fifoName.c_str(), 0777);
size_t duration = 50;

        while (!g_terminated)
        {
            int fd = open(fifoName.c_str(), O_RDONLY);
            try
            {
                shared_ptr<Chunk> chunk;//(new WireChunk());
                while (true)//cin.good())
                {
                    chunk.reset(new Chunk(sampleRate, channels, bps, duration));//2*WIRE_CHUNK_SIZE));
					WireChunk* wireChunk = chunk->wireChunk;
                    int toRead = wireChunk->length;
//                    cout << "tr: " << toRead << ", size: " << WIRE_CHUNK_SIZE << "\t";
//                    char* payload = (char*)(&chunk->payload[0]);
                    int len = 0;
                    do
                    {
                        int count = read(fd, wireChunk->payload + len, toRead - len);
                        if (count <= 0)
                            throw ServerException("count = " + boost::lexical_cast<string>(count));

                        len += count;
                    }
                    while (len < toRead);

                    wireChunk->tv_sec = tvChunk.tv_sec;
                    wireChunk->tv_usec = tvChunk.tv_usec;
                    server->send(chunk);

                    addMs(tvChunk, duration);
                    nextTick += duration;
                    long currentTick = getTickCount();
                    if (nextTick > currentTick)
                    {
                        usleep((nextTick - currentTick) * 1000);
                    }
                    else
                    {
                        cin.sync();
                        gettimeofday(&tvChunk, NULL);
                        nextTick = getTickCount();
                    }
                }
            }
            catch(const std::exception& e)
            {
				std::cerr << "Exception: " << e.what() << std::endl;
            }
            close(fd);
        }

		server->stop();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << std::endl;
	}

	syslog (LOG_NOTICE, "First daemon terminated.");
    closelog();
}


