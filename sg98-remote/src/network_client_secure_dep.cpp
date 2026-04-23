#include <WiFiClientSecure.h>

// Force PlatformIO's dependency resolver to pull in the framework-provided
// NetworkClientSecure implementation. WebSocketsClient uses this type
// internally for TLS, but the framework library is not linked in this project
// layout unless the application references the header directly.
void sg98_network_client_secure_dependency_anchor() {}
