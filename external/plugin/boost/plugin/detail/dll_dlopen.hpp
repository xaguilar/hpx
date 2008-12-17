// Copyright Vladimir Prus 2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_DLL_DLOPEN_HPP_VP_2004_08_24
#define BOOST_DLL_DLOPEN_HPP_VP_2004_08_24

#include <string>
#include <stdexcept>
#include <iostream>

#include <boost/bind.hpp>
#include <boost/config.hpp>
#include <boost/static_assert.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/type_traits/remove_pointer.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/thread.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/convenience.hpp>

#include <boost/plugin/config.hpp>

#include <dlfcn.h> 

#if !defined(BOOST_HAS_DLOPEN)
#error "This file shouldn't be included directly, use the file boost/plugin/dll.hpp only."
#endif

typedef void* HMODULE;

///////////////////////////////////////////////////////////////////////////////
#if !defined(RTLD_LOCAL)
#define RTLD_LOCAL 0        // some systems do not have RTLD_LOCAL
#endif
#if !defined(RTLD_DEEPBIND)
#define RTLD_DEEPBIND 0     // some systems do not have RTLD_DEEPBIND
#endif

///////////////////////////////////////////////////////////////////////////////
#define MyFreeLibrary(x)      dlclose (x)
#define MyLoadLibrary(x)      dlopen  (x, RTLD_GLOBAL | RTLD_LAZY)
#define MyGetProcAddress(x,y) dlsym   (x, y)

///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace plugin {

    ///////////////////////////////////////////////////////////////////////////
    class dll 
    {
    protected:
        ///////////////////////////////////////////////////////////////////////
        static void init_library(HMODULE dll_handle)
        {
#if defined(__AIX__) && defined(__GNUC__)
            typedef void (*init_proc_type)();
            init_proc_type init_proc = 
                (init_proc_type)MyGetProcAddress(dll_handle, "_GLOBAL__DI");
            if (init_proc)
                init_proc();
#endif
        }

        static void deinit_library(HMODULE dll_handle)
        {
#if defined(__AIX__) && defined(__GNUC__)
            typedef void (*free_proc_type)();
            free_proc_type free_proc = 
                (free_proc_type)MyGetProcAddress(dll_handle, "_GLOBAL__DD");
            if (free_proc)
                free_proc();
#endif
        }

        ///////////////////////////////////////////////////////////////////////
        struct free_dll 
        {
            free_dll(HMODULE h) : h(h) {}

            template<typename T>
            void operator()(T)
            {
                if (NULL != h)
                {
                    dll::initialize_mutex();
                    boost::mutex::scoped_lock lock(dll::mutex_instance());

                    dll::deinit_library(h);
                    MyFreeLibrary(h);
                }
            }

            HMODULE h;
        };
        friend struct free_dll;

    public:
        dll() 
        :   dll_handle (NULL) 
        {
            LoadLibrary();
        }

        dll(dll const& rhs) 
        :   dll_name(rhs.dll_name), map_name(rhs.map_name), dll_handle(NULL)
        {
            LoadLibrary();
        }

        dll(std::string const& name) 
        :   dll_name(name), map_name(""), dll_handle(NULL)
        {
            // map_name defaults to dll base name
            namespace fs = boost::filesystem;

            fs::path dll_path(dll_name, fs::native);
            map_name = fs::basename(dll_path);

            LoadLibrary();
        }

        dll(std::string const& libname, std::string const& mapname) 
        :   dll_name(libname), map_name(mapname), dll_handle(NULL)
        {
            LoadLibrary();
        }

        dll &operator=(dll const& rhs)
        {
            if (this != &rhs) {
            //  free any existing dll_handle
                FreeLibrary();

            //  load the library for this instance of the dll class
                dll_name = rhs.dll_name;
                map_name = rhs.map_name;
                LoadLibrary();
            }
            return *this;
        }

        ~dll() 
        { 
            FreeLibrary();
        }

        std::string get_name() const { return dll_name; }
        std::string get_mapname() const { return map_name; }

        template<typename SymbolType, typename Deleter>
        std::pair<SymbolType, Deleter> 
        get(std::string const& symbol_name) const
        {
            initialize_mutex();
            boost::mutex::scoped_lock lock(mutex_instance());

            BOOST_STATIC_ASSERT(boost::is_pointer<SymbolType>::value);
            typedef typename remove_pointer<SymbolType>::type PointedType;
            dlerror();

            // Open the library. Yes, we do it on every access to 
            // a symbol, the LoadLibrary function increases the refcnt of the dll
            // so in the end the dll class holds one refcnt and so does every 
            // symbol.
            HMODULE handle = MyLoadLibrary((dll_name.empty() ? NULL : dll_name.c_str()));
            if (!handle) {
                BOOST_PLUGIN_OSSTREAM str;
                str << "Boost.Plugin: Could not open shared library '" 
                    << dll_name << ": dlerror: " << dlerror();

                throw std::logic_error(BOOST_PLUGIN_OSSTREAM_GETSTRING(str));
            }

#if !defined(__AIX__) 
            // AIX seems to return different handle values for the second and 
            // any following call
            BOOST_ASSERT(handle == dll_handle);
#endif

            dlerror();              // Clear the error state.
            init_library(handle);   // initialize library

            // Cast the to right type.
            SymbolType address = (SymbolType)MyGetProcAddress(dll_handle, symbol_name.c_str());
            if (NULL == address) 
            {
                BOOST_PLUGIN_OSSTREAM str;
                str << "Boost.Plugin: Unable to locate the exported symbol name '" 
                    << symbol_name << "' in the shared library '" 
                    << dll_name << "' (" << dlerror () << ")";
                    
                throw std::logic_error(BOOST_PLUGIN_OSSTREAM_GETSTRING(str));
            }
            return std::make_pair(address, free_dll(handle));
        }

    protected:
        void LoadLibrary()
        {
            initialize_mutex();
            boost::mutex::scoped_lock lock(mutex_instance());

            dlerror();
            dll_handle = MyLoadLibrary((dll_name.empty() ? NULL : dll_name.c_str()));
            // std::cout << "open\n";
            if (!dll_handle) {
                BOOST_PLUGIN_OSSTREAM str;
                str << "Boost.Plugin: Could not open shared library '" 
                    << dll_name << ": dlerror: " << dlerror();
                throw std::logic_error(BOOST_PLUGIN_OSSTREAM_GETSTRING(str));
            }
            init_library(dll_handle);    // initialize library
        }
        
        void FreeLibrary()
        {
            if (NULL != dll_handle) 
            {
                initialize_mutex();
                boost::mutex::scoped_lock lock(mutex_instance());

                deinit_library(dll_handle);                
                MyFreeLibrary(dll_handle); 
            }
        }

    // protect access to dl... functions
        static boost::mutex &mutex_instance()
        {
            static boost::mutex mutex;
            return mutex;
        }
        static void mutex_init()
        {
            mutex_instance();
        }
        static void initialize_mutex()
        {
            static boost::once_flag been_here = BOOST_ONCE_INIT;
            boost::call_once(mutex_init, been_here);
        }

    private:
        std::string dll_name;
        std::string map_name;
        HMODULE dll_handle;
    };

///////////////////////////////////////////////////////////////////////////////
}}

#endif

