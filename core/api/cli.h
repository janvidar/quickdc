namespace QuickDC {
namespace CLI {

struct CommandTemplate {
	const char* cmd;
	size_t     length;
	size_t     params;
	const char* parse_args;
	const char* desc_args;
	const char* description;
};

class ParsedCommand {
	public:
		ParsedCommand();
		virtual ~ParsedCommand();
		
	protected:
		std::vector<char*> arguments;
};




}
}