CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp  ./timer/tw_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./log/double_buffer.cpp ./CGImysql/sql_connection_pool.cpp  webserver.cpp config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm  -r server
