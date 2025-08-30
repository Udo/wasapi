#include "request.h"

Request::Request(Arena* ar)
{
	arena = ar;
	env = DynamicVariable::make_object();
	env["DBG_ARENA"] = DynamicVariable::make_number(arena->management_flag);
	params = DynamicVariable::make_object();
	cookies = DynamicVariable::make_object();
	headers = DynamicVariable::make_object();
	files = DynamicVariable::make_array();
	session = DynamicVariable::make_object();
	context = DynamicVariable::make_object();
}
