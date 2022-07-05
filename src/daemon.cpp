#include <vector>
#include <iostream>
#include <thread>
#include <boost/program_options.hpp>
#include <xapian.h>
#include <zmq.hpp>

#include "debug.hpp"
#include "project_config.hpp"

#define ELPP_FEATURE_CRASH_LOG
#include "easylogging++.h"

using namespace neroshop;

#define NEROMON_TAG std::string("\033[1;95m[neromon]: \033[0m")

// *****************************************************************************
void interpret_query( std::string&& query ) {
  if (query[0] == 'd' && query[1] == 'b') {
     query.erase( 0, 3 );
     LOG(DEBUG) << "Processing db '" << query << "'";
  }
}

// *****************************************************************************
void run_server( zmq::socket_t&& socket ) {
  el::Helpers::setThreadName("server");
  LOG(INFO) << el::Helpers::getThreadName() << " thread initialized";
  while(true) {
    zmq::message_t request;
    auto res = socket.recv( request, zmq::recv_flags::none );
    LOG(DEBUG) << "Received " << request.to_string();
    socket.send( zmq::buffer("accept"), zmq::send_flags::none );
    interpret_query( request.to_string() );
  }
}

// *****************************************************************************
void run_database( const std::string& db_name ) {
  el::Helpers::setThreadName("db");
  LOG(INFO) << el::Helpers::getThreadName() << " thread initialized";
  Xapian::WritableDatabase db( db_name, Xapian::DB_CREATE_OR_OPEN );
  Xapian::TermGenerator indexer;
  Xapian::Stem stemmer( "english" );
  indexer.set_stemmer( stemmer );
  indexer.set_stemming_strategy( indexer.STEM_SOME_FULL_POS );
}

void crash_handler( int sig ) {
  if (sig == SIGINT) {
    LOG(INFO) << "Ctrl-C pressed, " << neroshop::daemon_executable()
              << " exiting";
  } else {
    LOG(ERROR) << "Crashed!";
    el::Helpers::logCrashReason( sig, true );
  }
  // FOLLOWING LINE IS ABSOLUTELY NEEDED TO ABORT APPLICATION
  el::Helpers::crashAbort( sig );
}

// *****************************************************************************
// neroshop daemon (neromon) main
int main( int argc, char **argv ) {

  // Configure logging
  el::Configurations c;
  c.setGlobally( el::ConfigurationType::Format,
    "%datetime %host " + neroshop::daemon_executable() + "[%thread]: %level: %msg");
  c.setGlobally( el::ConfigurationType::Filename, "neromon.log" );
  c.parseFromText( "* GLOBAL:\n  ENABLED = true\n"
                                "TO_FILE = true\n"
                                "TO_STANDARD_OUTPUT = true\n" );
  el::Loggers::setDefaultConfigurations( c, true );
  el::Loggers::addFlag( el::LoggingFlag::ColoredTerminalOutput );
  el::Helpers::setThreadName( "main" );
  el::Helpers::setCrashHandler( crash_handler );

  // Display initial info
  std::string version( "Neroshop: " + neroshop::daemon_executable() + " v"
                       + neroshop::project_version() + "-"
                       + neroshop::build_type() );

  std::string welcome( "This is the daemon of neroshop. It can run standalone "
    "or as a daemon in the\nbackground using --detach. You can use " +
     neroshop::cli_executable() + " to interact with it." );

  std::string usage( "Usage: " + neroshop::daemon_executable() + " [OPTIONS]" );

  // Defaults
  int server_port = 1234;
  std::string db_name( "neroshop_test" );

  // Supported command line arguments
  namespace po = boost::program_options;
  po::options_description desc( "Options" );
  std::string port_help( "Listen on custom port, default: " );
  port_help += std::to_string( server_port );
  std::string db_help( "Use database, default: " + db_name );
  desc.add_options()
    ("help", "Show help message")
    ("version", "Show version information")
    ("license", "Show license information")
    ("detach", "Run as a daemon in the background")
    ("port", po::value<int>(), port_help.c_str())
    ("db", po::value<std::string>(), db_help.c_str())
  ;

  po::variables_map vm;
  po::store( po::parse_command_line(argc, argv, desc), vm );
  po::notify( vm );

  if (vm.count( "help" )) {

    std::cout << version << "\n\n" << welcome << "\n\n" << usage << "\n\n"
              << desc << '\n';
    return 1;

  } else if (vm.count( "version" )) {

    std::cout << version << '\n' << neroshop::copyright() << '\n';
    return 1;

  } else if (vm.count( "license" )) {

    std::cout << version << '\n' << neroshop::license() << '\n';
    return 1;

  } if (vm.count( "port" )) {

    server_port = vm[ "port" ].as< int >();

  } if (vm.count( "db" )) {

    db_name = vm[ "db" ].as< std::string >();

  }

  if (db_name.empty()) {
    std::cerr << "Need a database name\n";
    return -1;
  }

  LOG(INFO) << "Start " << version;

  if (vm.count( "detach" )) {

    std::cout << "Running in daemon mode.\n";

    // Fork the current process. The parent process continues with a process ID
    // greater than 0.  A process ID lower than 0 indicates a failure in either
    // process.
    pid_t pid = fork();
    if (pid > 0) {
      std::cout << NEROMON_TAG << "\033[1;97mForked PID: \033[0;32m" << pid
                << "\033[0m" << std::endl;
      return EXIT_SUCCESS;
    } else if (pid < 0) {
      return EXIT_FAILURE;
    }

    // Generate a session ID for the child process and ensure it is valid.
    if(setsid() < 0) {
      // Log failure and exit
      std::cerr << "Could not generate session ID for child process\n";
      // If a new session ID could not be generated, we must terminate the child
      // process or it will be orphaned.
      return EXIT_FAILURE;
    }

  } else {

    LOG(INFO) << "Running in standalone mode";

  }

  LOG(INFO) << "Using database: " << db_name;

  // initialize the zmq context with a single IO thread
  zmq::context_t context{ 1 };
  // construct a REP (reply) socket and bind to interface
  zmq::socket_t socket{ context, zmq::socket_type::rep };
  socket.bind( "tcp://*:" + std::to_string(server_port) );

  LOG(INFO) << "Server bound to port " << std::to_string(server_port);

  // start some threads
  std::vector< std::thread > daemon_threads;
  daemon_threads.emplace_back( run_server, std::move(socket) );
  daemon_threads.emplace_back( run_database, db_name );

  // wait for all threads to finish
  for (auto& t : daemon_threads) t.join();

  // Terminate the child process when the daemon completes
  return EXIT_SUCCESS;
}
