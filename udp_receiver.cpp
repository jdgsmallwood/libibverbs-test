#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PORT 12345
#define BUFFER_SIZE 4096
#define RING_BUFFER_SIZE 1000
#define MIN_PCAP_HEADER_SIZE 58

// Your custom headers (same as client)
#pragma pack(push, 1)
struct EthernetHeader {
  uint8_t dst[6];
  uint8_t src[6];
  uint16_t ethertype;
};

struct IPHeader {
  uint8_t version_ihl;
  uint8_t dscp_ecn;
  uint16_t total_length;
  uint16_t identification;
  uint16_t flags_fragment;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t header_checksum;
  uint32_t src_ip;
  uint32_t dst_ip;
};

struct UDPHeader {
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t length;
  uint16_t checksum;
};

struct CustomHeader {
  uint64_t sample_count;
  uint32_t fpga_id;
  uint16_t freq_channel;
  uint8_t padding[8];
};
#pragma pack(pop)

// Packet storage for ring buffer
struct PacketEntry {
  uint8_t data[BUFFER_SIZE];
  int length;
  struct sockaddr_in sender_addr;
  struct timeval timestamp;
  int processed; // 0 = unprocessed, 1 = processed
};

// Processed packet info
struct ProcessedPacket {
  uint64_t sample_count;
  uint32_t fpga_id;
  uint16_t freq_channel;
  uint8_t *payload;
  int payload_size;
  struct timeval timestamp;
};

// Global ring buffer
static struct PacketEntry ring_buffer[RING_BUFFER_SIZE];
static int write_index = 0;
static int read_index = 0;
static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static int running = 1;

// Statistics
static std::atomic<unsigned long long> packets_received = 0;
static std::atomic<unsigned long long> packets_processed = 0;

int get_next_write_index() { return (write_index + 1) % RING_BUFFER_SIZE; }

int get_next_read_index() { return (read_index + 1) % RING_BUFFER_SIZE; }

int buffer_has_data() { return read_index != write_index; }

int buffer_is_full() { return get_next_write_index() == read_index; }

void store_packet(uint8_t *data, int length, struct sockaddr_in *sender) {
  pthread_mutex_lock(&buffer_mutex);

  if (buffer_is_full()) {
    printf("Warning: Ring buffer full, dropping packet\n");
    pthread_mutex_unlock(&buffer_mutex);
    return;
  }

  struct PacketEntry *entry = &ring_buffer[write_index];
  memcpy(entry->data, data, length);
  entry->length = length;
  entry->sender_addr = *sender;
  gettimeofday(&entry->timestamp, NULL);
  entry->processed = 0;

  write_index = get_next_write_index();
  packets_received.fetch_add(1, std::memory_order_relaxed);

  pthread_mutex_unlock(&buffer_mutex);
}

struct PacketEntry *get_next_packet() {
  pthread_mutex_lock(&buffer_mutex);

  if (!buffer_has_data()) {
    pthread_mutex_unlock(&buffer_mutex);
    return NULL;
  }

  struct PacketEntry *entry = &ring_buffer[read_index];
  read_index = get_next_read_index();

  pthread_mutex_unlock(&buffer_mutex);
  return entry;
}

struct ProcessedPacket parse_custom_packet(struct PacketEntry *entry) {
  struct ProcessedPacket result = {0};

  if (entry->length < MIN_PCAP_HEADER_SIZE) {
    printf("Packet too small for custom headers\n");
    return result;
  }

  // Parse your custom packet structure
  const EthernetHeader *eth = (const EthernetHeader *)entry->data;
  if (ntohs(eth->ethertype) != 0x0800) {
    printf("Not IPv4 packet\n");
    return result;
  }

  const CustomHeader *custom = (const CustomHeader *)(entry->data + 42);

  result.sample_count = custom->sample_count;
  result.fpga_id = custom->fpga_id;
  result.freq_channel = custom->freq_channel;
  result.timestamp = entry->timestamp;

  // Point to payload (after headers)
  result.payload = entry->data + MIN_PCAP_HEADER_SIZE;
  result.payload_size = entry->length - MIN_PCAP_HEADER_SIZE;

  return result;
}

void process_packet_data(struct ProcessedPacket *pkt) {
  // This is where you'd do your actual processing
  // For now, just print the info and simulate some work

  printf("Processing packet: sample_count=%lu, freq_channel=%u, fpga_id=%u, "
         "payload=%d bytes\n",
         pkt->sample_count, pkt->freq_channel, pkt->fpga_id, pkt->payload_size);

  // Simulate processing time
  usleep(50000); // 50ms processing time

  // Here you could:
  // - Copy payload to your processing arrays
  // - Call your existing process_packet() function
  // - Pass to further processing stages

  packets_processed.fetch_add(1, std::memory_order_relaxed);
}

// Receiver thread - continuously receives packets
void *receiver_thread(void *arg) {
  int sockfd = *(int *)arg;
  uint8_t buffer[BUFFER_SIZE];
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  printf("Receiver thread started\n");

  while (running) {
    int received = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                            (struct sockaddr *)&client_addr, &client_len);

    if (received < 0) {
      if (errno == EINTR)
        continue;
      perror("recvfrom");
      break;
    }

    // Store in ring buffer
    store_packet(buffer, received, &client_addr);
  }

  printf("Receiver thread exiting\n");
  return NULL;
}

// Processor thread - continuously processes packets
void *processor_thread(void *arg) {
  printf("Processor thread started\n");

  while (running) {
    struct PacketEntry *entry = get_next_packet();

    if (entry == NULL) {
      usleep(10); // 10us sleep when no data
      continue;
    }

    struct ProcessedPacket parsed = parse_custom_packet(entry);

    if (parsed.payload_size > 0) {
      process_packet_data(&parsed);
    }
  }

  printf("Processor thread exiting\n");
  return NULL;
}

int main() {
  int sockfd;
  struct sockaddr_in server_addr;
  pthread_t receiver_tid, processor_tid;

  printf("UDP Server with concurrent processing starting on port %d...\n",
         PORT);
  printf("Ring buffer size: %d packets\n\n", RING_BUFFER_SIZE);

  // Create UDP socket
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return 1;
  }

  // Allow address reuse
  int reuse = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt");
  }

  // Setup server address
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  // Bind socket
  if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    close(sockfd);
    return 1;
  }

  printf("Server listening on 0.0.0.0:%d\n", PORT);
  printf("Press Ctrl+C to stop\n\n");

  // Start receiver thread
  if (pthread_create(&receiver_tid, NULL, receiver_thread, &sockfd) != 0) {
    perror("pthread_create receiver");
    close(sockfd);
    return 1;
  }

  // Start processor thread
  if (pthread_create(&processor_tid, NULL, processor_thread, NULL) != 0) {
    perror("pthread_create processor");
    running = 0;
    pthread_join(receiver_tid, NULL);
    close(sockfd);
    return 1;
  }

  // Print statistics periodically
  while (running) {
    sleep(5);
    printf(
        "Stats: Received=%llu, Processed=%llu, Buffer usage=%d/%d\n",
        (unsigned long long)packets_received.load(std::memory_order_relaxed),
        (unsigned long long)packets_processed.load(std::memory_order_relaxed),
        (write_index - read_index + RING_BUFFER_SIZE) % RING_BUFFER_SIZE,
        RING_BUFFER_SIZE);
  }

  // Cleanup
  printf("\nShutting down...\n");
  running = 0;
  pthread_join(receiver_tid, NULL);
  pthread_join(processor_tid, NULL);

  close(sockfd);
  return 0;
}
