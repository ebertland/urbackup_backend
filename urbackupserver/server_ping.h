#include "../Interface/Thread.h"
#include "../Interface/Mutex.h"

class ClientMain;

class ServerPingThread : public IThread
{
public:
	ServerPingThread(ClientMain *client_main, const std::string& clientname, size_t status_id, bool with_eta);
	void operator()(void);
	void setStop(bool b);

	bool isTimeout(void);

private:
	ClientMain *client_main;
	volatile bool stop;
	volatile bool is_timeout;
	bool with_eta;
	const std::string& clientname;
	size_t status_id;
};