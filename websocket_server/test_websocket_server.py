#!/usr/bin/env python3
"""
Simple and stable WebSocket server for testing
"""

import asyncio
import websockets
import logging

logging.basicConfig(level=logging.INFO, format='[%(asctime)s] %(levelname)s: %(message)s', datefmt='%H:%M:%S')
logger = logging.getLogger(__name__)

async def echo_handler(websocket, path=None):
    """Simple echo handler"""
    try:
        logger.info(f"New connection from {websocket.remote_address}")
        
        # Send welcome message
        await websocket.send("Welcome to Python WebSocket Server!")
        logger.info("Sent welcome message")
        
        # Echo loop
        async for message in websocket:
            logger.info(f"Received: {message}")
            response = f"Echo: {message}"
            await websocket.send(response)
            logger.info(f"Sent: {response}")
            
    except websockets.exceptions.ConnectionClosed:
        logger.info("Connection closed normally")
    except Exception as e:
        logger.error(f"Error: {e}")

async def main():
    """Main server function"""
    host = "localhost"
    port = 9001
    
    logger.info(f"Starting WebSocket server on {host}:{port}")
    
    try:
        # Start server
        server = await websockets.serve(echo_handler, host, port)
        logger.info(f"Server running on ws://{host}:{port}")
        
        # Keep running
        await asyncio.Future()
        
    except KeyboardInterrupt:
        logger.info("Server stopped by user")
    except Exception as e:
        logger.error(f"Server error: {e}")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server shutdown complete")