#!/bin/bash

set -e

DRIVER_NAME="mgpu"
DEVICE_NODE="/dev/mgpu0"
DEBUGFS_ROOT="/sys/kernel/debug/mgpu"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Helper functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_root() {
    if [ "$EUID" -ne 0 ]; then 
        log_error "Please run as root"
        exit 1
    fi
}

# Load the driver
load_driver() {
    log_info "Loading MGPU driver..."
    
    if lsmod | grep -q "^$DRIVER_NAME"; then
        log_warn "Driver already loaded, unloading first..."
        rmmod $DRIVER_NAME
        sleep 1
    fi
    
    # Load with debug and self test enabled
    insmod mgpu.ko run_selftests=1 || {
        log_error "Failed to load driver"
        dmesg | tail -20
        exit 1
    }
    
    sleep 1
    
    # Check if device node was created
    if [ ! -e "$DEVICE_NODE" ]; then
        log_error "Device node $DEVICE_NODE not created"
        exit 1
    fi
    
    log_info "Driver loaded successfully"
}

# Check driver status
check_status() {
    log_info "Checking driver status..."
    
    # Check dmesg for errors
    if dmesg | tail -50 | grep -q "mgpu.*error"; then
        log_warn "Errors found in dmesg:"
        dmesg | tail -50 | grep "mgpu"
    fi
    
    # Check if debugfs is available
    if [ -d "$DEBUGFS_ROOT" ]; then
        log_info "DebugFS available at $DEBUGFS_ROOT"
        
        # Read version
        if [ -f "$DEBUGFS_ROOT/version" ]; then
            VERSION=$(cat $DEBUGFS_ROOT/version)
            log_info "GPU Version: $VERSION"
        fi
        
        # Read capabilities
        if [ -f "$DEBUGFS_ROOT/caps" ]; then
            CAPS=$(cat $DEBUGFS_ROOT/caps)
            log_info "GPU Capabilities: $CAPS"
        fi
        
        # List all debugfs files
        log_info "Available debug files:"
        ls -la $DEBUGFS_ROOT/
    else
        log_warn "DebugFS not available"
    fi
}

# Test basic IOCTL operations
test_ioctls() {
    log_info "Testing IOCTL operations..."
    
    # This would require a test program
    # For now, just check if device can be opened
    if dd if=$DEVICE_NODE of=/dev/null bs=1 count=0 2>/dev/null; then
        log_info "Device can be opened"
    else
        log_error "Cannot open device"
        exit 1
    fi
}

# Test memory allocation
test_memory() {
    log_info "Testing memory allocation..."
    
    # Check CMA allocation
    if [ -f /proc/meminfo ]; then
        CMA_TOTAL=$(grep "CmaTotal" /proc/meminfo | awk '{print $2}')
        CMA_FREE=$(grep "CmaFree" /proc/meminfo | awk '{print $2}')
        log_info "CMA Total: ${CMA_TOTAL}kB, Free: ${CMA_FREE}kB"
    fi
}

# Monitor interrupts
monitor_interrupts() {
    log_info "Monitoring interrupts..."
    
    IRQ_LINE=$(grep mgpu /proc/interrupts | awk '{print $1}' | sed 's/://')
    if [ -n "$IRQ_LINE" ]; then
        log_info "MGPU using IRQ $IRQ_LINE"
        
        # Get initial count
        INITIAL_COUNT=$(grep mgpu /proc/interrupts | awk '{print $2}')
        log_info "Initial interrupt count: $INITIAL_COUNT"
        
        # Wait and check again
        sleep 2
        
        FINAL_COUNT=$(grep mgpu /proc/interrupts | awk '{print $2}')
        log_info "Final interrupt count: $FINAL_COUNT"
        
        if [ "$FINAL_COUNT" -gt "$INITIAL_COUNT" ]; then
            log_info "Interrupts are firing"
        else
            log_warn "No interrupts detected"
        fi
    else
        log_warn "No IRQ line found for MGPU"
    fi
}

# Stress test
stress_test() {
    log_info "Running stress test..."
    
    # Rapid load/unload
    for i in {1..5}; do
        log_info "Stress iteration $i/5"
        rmmod $DRIVER_NAME 2>/dev/null || true
        sleep 0.5
        insmod mgpu.ko
        sleep 0.5
    done
    
    log_info "Stress test completed"
}

# Cleanup
cleanup() {
    log_info "Cleaning up..."
    rmmod $DRIVER_NAME 2>/dev/null || true
    log_info "Driver unloaded"
}

# Main test sequence
main() {
    log_info "=== MGPU Driver Test Suite ==="
    
    check_root
    
    # Clear dmesg for clean output
    dmesg -C
    
    # Run tests
    load_driver
    check_status
    test_ioctls
    test_memory
    monitor_interrupts
    
    # Optional stress test
    if [ "$1" == "--stress" ]; then
        stress_test
    fi
    
    # Show final dmesg
    log_info "=== Driver Messages ==="
    dmesg | grep mgpu || log_warn "No driver messages found"
    
    # Cleanup
    if [ "$1" != "--keep" ]; then
        cleanup
    else
        log_info "Driver left loaded (--keep specified)"
    fi
    
    log_info "=== Test Complete ==="
}

# Handle Ctrl+C
trap cleanup INT

# Run main function
main "$@"