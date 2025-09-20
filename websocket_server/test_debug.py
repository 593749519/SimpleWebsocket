#!/usr/bin/env python3
"""
Debug WebSocket test for large payloads.
"""

import websocket
import time

def test_large_payload(size_bytes, description):
    """Test sending a large payload with detailed logging."""
    print(f"\n--- Testing {description} ({size_bytes:,} bytes) ---")
    
    try:
        # Create WebSocket connection
        print("Connecting to server...")
        ws = websocket.create_connection("ws://localhost:9001/", timeout=30)  # Increased timeout
        print("✓ Connected to server")
        
        # Create and send payload
        print(f"Creating payload of {size_bytes:,} bytes...")
        payload = "A" * size_bytes
        print("✓ Payload created")
        
        print("Sending payload...")
        start_time = time.time()
        ws.send(payload)
        send_time = time.time() - start_time
        print(f"✓ Sent {size_bytes:,} bytes in {send_time:.3f}s")
        
        # Try to receive response
        print("Waiting for response...")
        try:
            response = ws.recv()
            response_time = time.time() - start_time
            print(f"✓ Received response in {response_time:.3f}s total")
            print(f"  Response length: {len(response)} bytes")
            print(f"  Response: {response[:100]}{'...' if len(response) > 100 else ''}")
            ws.close()
            return True
            
        except websocket.WebSocketTimeoutException:
            print("✗ Timeout waiting for response")
            ws.close()
            return False
        except Exception as e:
            print(f"✗ Error receiving response: {e}")
            ws.close()
            return False
            
    except Exception as e:
        print(f"✗ Connection failed: {e}")
        return False

def main():
    print("Debug WebSocket Large Payload Test")
    print("=" * 50)
    
    # Test specific sizes that were failing
    test_cases = [
        (1024 * 1024, "1MB"),
        (5 * 1024 * 1024, "5MB"),
        (10 * 1024 * 1024, "10MB"),
    ]
    
    for size, description in test_cases:
        success = test_large_payload(size, description)
        if not success:
            print(f"❌ {description} test failed!")
        else:
            print(f"✅ {description} test passed!")
        
        time.sleep(1)  # Wait between tests
    
    print("\n" + "=" * 50)
    print("TEST COMPLETED")

if __name__ == "__main__":
    main()
